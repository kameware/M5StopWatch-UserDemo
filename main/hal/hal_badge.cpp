/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "utils/config_ap/config_ap.h"

#include <assets/assets.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <mooncake_log.h>

static const std::string_view _tag = "HAL-Badge";

namespace {

constexpr const char* _badge_dir            = "/spiflash/badge";
constexpr const char* _active_slot_file     = "/spiflash/badge/active_slot.txt";
constexpr std::size_t _max_badge_slot_count = 6;
constexpr std::size_t _max_animation_frames = 24;
constexpr uint16_t _max_animation_size      = 466;
constexpr uint16_t _min_animation_delay_ms  = 40;
constexpr uint16_t _max_animation_delay_ms  = 1000;
constexpr std::size_t _decoded_badge_cache_max_bytes     = 6 * 1024 * 1024;
constexpr std::size_t _decoded_badge_cache_min_free_bytes = 512 * 1024;
constexpr char _animation_magic[]           = {'B', 'A', 'N', 'I', 'M', '1'};

struct BadgeAnimationState {
    lv_timer_t* timer = nullptr;
    lv_obj_t* image   = nullptr;
    std::vector<std::string> frame_paths;
    std::size_t frame_index = 0;
    uint16_t delay_ms       = 125;
};

struct DecodedBadgeImage {
    std::string key;
    uint8_t* pixels = nullptr;
    std::size_t bytes = 0;
    lv_image_dsc_t image = {};
    uint32_t last_used = 0;
    bool stale = false;

    DecodedBadgeImage() = default;
    DecodedBadgeImage(const DecodedBadgeImage&) = delete;
    DecodedBadgeImage& operator=(const DecodedBadgeImage&) = delete;

    ~DecodedBadgeImage()
    {
        if (pixels != nullptr) {
            heap_caps_free(pixels);
        }
    }
};

BadgeAnimationState _animation_state;
std::vector<std::unique_ptr<DecodedBadgeImage>> _decoded_badge_cache;
std::vector<std::string> _badge_preload_queue;
std::size_t _decoded_badge_cache_bytes = 0;
uint32_t _decoded_badge_cache_clock    = 0;
DecodedBadgeImage* _visible_decoded_badge_image = nullptr;
lv_timer_t* _badge_preload_timer                 = nullptr;

std::string slot_meta_path(std::size_t slot)
{
    char path[96] = {};
    snprintf(path, sizeof(path), "%s/slot_%u.meta", _badge_dir, static_cast<unsigned>(slot));
    return std::string(path);
}

std::string slot_image_path(std::size_t slot, std::string_view extension)
{
    char path[96] = {};
    snprintf(path, sizeof(path), "%s/slot_%u.%.*s", _badge_dir, static_cast<unsigned>(slot),
             static_cast<int>(extension.size()), extension.data());
    return std::string(path);
}

std::string slot_frames_dir(std::size_t slot)
{
    char path[96] = {};
    snprintf(path, sizeof(path), "%s/slot_%u_frames", _badge_dir, static_cast<unsigned>(slot));
    return std::string(path);
}

std::string slot_frame_path(std::size_t slot, std::size_t frame_index)
{
    char path[128] = {};
    snprintf(path, sizeof(path), "%s/%03u.jpg", slot_frames_dir(slot).c_str(), static_cast<unsigned>(frame_index));
    return std::string(path);
}

std::string lvgl_file_path(const std::string& fs_path)
{
    return std::string("A:") + fs_path;
}

bool path_has_extension(const std::string& path, const char* extension)
{
    const std::size_t ext_len = strlen(extension);
    return path.size() >= ext_len && path.compare(path.size() - ext_len, ext_len, extension) == 0;
}

std::size_t decoded_badge_frame_bytes()
{
    return static_cast<std::size_t>(_max_animation_size) * _max_animation_size * sizeof(uint16_t);
}

DecodedBadgeImage* find_decoded_badge_image(const std::string& key)
{
    for (auto it = _decoded_badge_cache.rbegin(); it != _decoded_badge_cache.rend(); ++it) {
        DecodedBadgeImage* image = it->get();
        if (!image->stale && image->key == key) {
            image->last_used = ++_decoded_badge_cache_clock;
            return image;
        }
    }
    return nullptr;
}

bool evict_one_decoded_badge_image()
{
    std::size_t victim = _decoded_badge_cache.size();
    uint32_t oldest    = UINT32_MAX;
    bool found_stale   = false;

    for (std::size_t index = 0; index < _decoded_badge_cache.size(); ++index) {
        DecodedBadgeImage* image = _decoded_badge_cache[index].get();
        if (image == _visible_decoded_badge_image) {
            continue;
        }

        if (image->stale) {
            victim      = index;
            found_stale = true;
            break;
        }

        if (!found_stale && image->last_used < oldest) {
            oldest = image->last_used;
            victim = index;
        }
    }

    if (victim >= _decoded_badge_cache.size()) {
        return false;
    }

    _decoded_badge_cache_bytes -= std::min(_decoded_badge_cache_bytes, _decoded_badge_cache[victim]->bytes);
    _decoded_badge_cache.erase(_decoded_badge_cache.begin() + static_cast<std::ptrdiff_t>(victim));
    return true;
}

void reserve_decoded_badge_cache_space(std::size_t needed_bytes)
{
    while (_decoded_badge_cache_bytes + needed_bytes > _decoded_badge_cache_max_bytes) {
        if (!evict_one_decoded_badge_image()) {
            break;
        }
    }

    while (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) <
           needed_bytes + _decoded_badge_cache_min_free_bytes) {
        if (!evict_one_decoded_badge_image()) {
            break;
        }
    }
}

void mark_decoded_badge_cache_stale_for_slot(std::size_t slot)
{
    const std::string image_prefix = slot_image_path(slot, "");
    const std::string frame_prefix = slot_frames_dir(slot) + "/";

    for (auto& entry : _decoded_badge_cache) {
        if (entry->key.rfind(image_prefix, 0) == 0 || entry->key.rfind(frame_prefix, 0) == 0) {
            entry->stale = true;
        }
    }
}

DecodedBadgeImage* decode_badge_image_to_cache(const std::string& fs_path)
{
    if (DecodedBadgeImage* cached = find_decoded_badge_image(fs_path)) {
        return cached;
    }

    const std::size_t bytes = decoded_badge_frame_bytes();
    reserve_decoded_badge_cache_space(bytes);

    auto* pixels = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    while (pixels == nullptr && evict_one_decoded_badge_image()) {
        pixels = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (pixels == nullptr) {
        mclog::tagWarn(_tag, "failed to allocate badge decoded cache: {} bytes", bytes);
        return nullptr;
    }

    auto& display = GetHAL().getDisplay();
    M5Canvas canvas(&display);
    canvas.setColorDepth(16);
    canvas.setBuffer(pixels, _max_animation_size, _max_animation_size, 16);
    canvas.fillSprite(TFT_BLACK);

    bool decoded = false;
    if (path_has_extension(fs_path, ".png")) {
        decoded = canvas.drawPngFile(fs_path.c_str(), 0, 0, _max_animation_size, _max_animation_size);
    } else {
        decoded = canvas.drawJpgFile(fs_path.c_str(), 0, 0, _max_animation_size, _max_animation_size);
    }
    canvas.deleteSprite();

    if (!decoded) {
        heap_caps_free(pixels);
        mclog::tagWarn(_tag, "failed to decode badge image into cache: {}", fs_path);
        return nullptr;
    }

    auto entry       = std::make_unique<DecodedBadgeImage>();
    entry->key       = fs_path;
    entry->pixels    = pixels;
    entry->bytes     = bytes;
    entry->last_used = ++_decoded_badge_cache_clock;

    // M5Canvas 16-bit sprites are byte-swapped RGB565 in memory.
    entry->image.header.cf     = LV_COLOR_FORMAT_RGB565_SWAPPED;
    entry->image.header.magic  = LV_IMAGE_HEADER_MAGIC;
    entry->image.header.w      = _max_animation_size;
    entry->image.header.h      = _max_animation_size;
    entry->image.header.stride = _max_animation_size * sizeof(uint16_t);
    entry->image.data_size     = static_cast<uint32_t>(bytes);
    entry->image.data          = pixels;

    _decoded_badge_cache_bytes += bytes;
    _decoded_badge_cache.push_back(std::move(entry));
    mclog::tagInfo(_tag, "decoded badge image cache: {}, total={}KB", fs_path, _decoded_badge_cache_bytes / 1024);
    return _decoded_badge_cache.back().get();
}

bool set_badge_image_source(lv_obj_t* image, const std::string& fs_path)
{
    if (image == nullptr) {
        return false;
    }

    if (DecodedBadgeImage* decoded = decode_badge_image_to_cache(fs_path)) {
        lv_image_set_src(image, &decoded->image);
        _visible_decoded_badge_image = decoded;
        return true;
    }

    const std::string path = lvgl_file_path(fs_path);
    lv_image_set_src(image, path.c_str());
    _visible_decoded_badge_image = nullptr;
    return true;
}

bool ensure_badge_dir()
{
    if (mkdir(_badge_dir, 0775) == 0 || errno == EEXIST) {
        return true;
    }

    mclog::tagError(_tag, "failed to create badge dir: {}, errno={}", _badge_dir, errno);
    return false;
}

bool ensure_slot_frames_dir(std::size_t slot)
{
    if (!ensure_badge_dir()) {
        return false;
    }

    const std::string dir = slot_frames_dir(slot);
    if (mkdir(dir.c_str(), 0775) == 0 || errno == EEXIST) {
        return true;
    }

    mclog::tagError(_tag, "failed to create badge frames dir: {}, errno={}", dir, errno);
    return false;
}

bool write_text_file(const std::string& path, std::string_view text)
{
    FILE* file = fopen(path.c_str(), "wb");
    if (file == nullptr) {
        mclog::tagError(_tag, "failed to open file for write: {}", path);
        return false;
    }

    const size_t written = fwrite(text.data(), 1, text.size(), file);
    fclose(file);
    if (written != text.size()) {
        mclog::tagError(_tag, "failed to write file: {}", path);
        return false;
    }

    return true;
}

bool write_binary_file(const std::string& path, const std::vector<uint8_t>& data)
{
    FILE* file = fopen(path.c_str(), "wb");
    if (file == nullptr) {
        mclog::tagError(_tag, "failed to open file for write: {}", path);
        return false;
    }

    const size_t written = data.empty() ? 0 : fwrite(data.data(), 1, data.size(), file);
    fclose(file);
    if (written != data.size()) {
        mclog::tagError(_tag, "failed to write file: {}", path);
        return false;
    }

    return true;
}

uint16_t read_le16(const std::vector<uint8_t>& data, std::size_t offset)
{
    return static_cast<uint16_t>(data[offset] | (static_cast<uint16_t>(data[offset + 1]) << 8));
}

uint32_t read_le32(const std::vector<uint8_t>& data, std::size_t offset)
{
    return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
}

bool is_jpeg_data(const std::vector<uint8_t>& data, std::size_t offset, std::size_t size)
{
    return size >= 4 && offset + size <= data.size() && data[offset] == 0xFF && data[offset + 1] == 0xD8 &&
           data[offset + size - 2] == 0xFF && data[offset + size - 1] == 0xD9;
}

std::string read_text_file(const std::string& path)
{
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return {};
    }

    char buffer[32]   = {};
    const size_t size = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);

    std::string text(buffer, size);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

std::string normalize_extension(std::string file_name, std::string content_type)
{
    auto normalize = [](std::string& text) {
        for (char& ch : text) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }
    };

    normalize(file_name);
    normalize(content_type);

    const auto dot = file_name.find_last_of('.');
    if (dot != std::string::npos) {
        const std::string ext = file_name.substr(dot + 1);
        if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" || ext == "banim") {
            return ext == "jpeg" ? "jpg" : ext;
        }
    }

    if (content_type.find("png") != std::string::npos) {
        return "png";
    }
    if (content_type.find("jpeg") != std::string::npos || content_type.find("jpg") != std::string::npos) {
        return "jpg";
    }
    if (content_type.find("gif") != std::string::npos) {
        return "gif";
    }
    if (content_type.find("x-m5-badge-animation") != std::string::npos ||
        content_type.find("badge-animation") != std::string::npos) {
        return "banim";
    }

    return {};
}

std::size_t read_active_slot()
{
    const std::string text = read_text_file(_active_slot_file);
    if (text.empty()) {
        return 0;
    }

    const unsigned long slot = strtoul(text.c_str(), nullptr, 10);
    if (slot >= _max_badge_slot_count) {
        mclog::tagWarn(_tag, "invalid active badge slot: {}", slot);
        return 0;
    }

    return static_cast<std::size_t>(slot);
}

std::string read_slot_extension(std::size_t slot)
{
    std::string ext = read_text_file(slot_meta_path(slot));
    if (ext == "png" || ext == "jpg" || ext == "gif" || ext == "banim") {
        return ext;
    }

    for (const auto* candidate : {"png", "jpg", "jpeg", "gif", "banim"}) {
        const std::string path = slot_image_path(slot, candidate);
        if (access(path.c_str(), R_OK) == 0) {
            return std::string(strcmp(candidate, "jpeg") == 0 ? "jpg" : candidate);
        }
    }

    return {};
}

std::string slot_content_type(std::string_view extension)
{
    if (extension == "jpg" || extension == "jpeg") {
        return "image/jpeg";
    }
    if (extension == "png") {
        return "image/png";
    }
    if (extension == "gif") {
        return "image/gif";
    }
    if (extension == "banim") {
        return "application/x-m5-badge-animation";
    }
    return "application/octet-stream";
}

bool read_binary_file(const std::string& path, std::vector<uint8_t>& data)
{
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    const long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return false;
    }
    rewind(file);

    data.resize(static_cast<std::size_t>(size));
    const size_t read_size = data.empty() ? 0 : fread(data.data(), 1, data.size(), file);
    fclose(file);
    return read_size == data.size();
}

bool is_slot_available(std::size_t slot)
{
    return !read_slot_extension(slot).empty();
}

bool persist_active_slot(std::size_t slot)
{
    if (slot >= _max_badge_slot_count) {
        return false;
    }

    return write_text_file(_active_slot_file, std::to_string(slot));
}

bool clear_active_slot()
{
    if (unlink(_active_slot_file) == 0 || errno == ENOENT) {
        return true;
    }
    return false;
}

bool read_animation_info(std::size_t slot, uint16_t& frame_count, uint16_t& delay_ms, uint16_t& width, uint16_t& height)
{
    const std::string text = read_text_file(slot_image_path(slot, "banim"));
    unsigned count        = 0;
    unsigned delay        = 0;
    unsigned image_width  = 0;
    unsigned image_height = 0;
    if (sscanf(text.c_str(), "%u %u %u %u", &count, &delay, &image_width, &image_height) != 4) {
        return false;
    }

    if (count == 0 || count > _max_animation_frames || delay < _min_animation_delay_ms ||
        delay > _max_animation_delay_ms || image_width == 0 || image_width > _max_animation_size ||
        image_height == 0 || image_height > _max_animation_size) {
        return false;
    }

    frame_count = static_cast<uint16_t>(count);
    delay_ms    = static_cast<uint16_t>(delay);
    width       = static_cast<uint16_t>(image_width);
    height      = static_cast<uint16_t>(image_height);
    return true;
}

bool is_slot_displayable(std::size_t slot)
{
    const std::string extension = read_slot_extension(slot);
    if (extension == "png" || extension == "jpg") {
        return true;
    }
    if (extension != "banim") {
        return false;
    }

    uint16_t frame_count = 0;
    uint16_t delay_ms    = 0;
    uint16_t width       = 0;
    uint16_t height      = 0;
    return read_animation_info(slot, frame_count, delay_ms, width, height) &&
           access(slot_frame_path(slot, 0).c_str(), R_OK) == 0;
}

std::size_t find_first_displayable_slot()
{
    for (std::size_t slot = 0; slot < _max_badge_slot_count; ++slot) {
        if (is_slot_displayable(slot)) {
            return slot;
        }
    }

    return _max_badge_slot_count;
}

std::size_t find_displayable_slot(std::size_t current_slot, int direction)
{
    if (direction == 0) {
        return _max_badge_slot_count;
    }

    for (std::size_t step = 1; step <= _max_badge_slot_count; ++step) {
        const std::size_t slot =
            static_cast<std::size_t>((static_cast<int>(current_slot) + direction * static_cast<int>(step) +
                                      static_cast<int>(_max_badge_slot_count)) %
                                     static_cast<int>(_max_badge_slot_count));
        if (is_slot_displayable(slot)) {
            return slot;
        }
    }

    return _max_badge_slot_count;
}

bool badge_preload_queue_contains(const std::string& fs_path)
{
    return std::find(_badge_preload_queue.begin(), _badge_preload_queue.end(), fs_path) != _badge_preload_queue.end();
}

void badge_preload_timer_cb(lv_timer_t*)
{
    while (!_badge_preload_queue.empty()) {
        const std::string fs_path = _badge_preload_queue.front();
        _badge_preload_queue.erase(_badge_preload_queue.begin());

        if (find_decoded_badge_image(fs_path) != nullptr) {
            continue;
        }

        decode_badge_image_to_cache(fs_path);
        break;
    }

    if (_badge_preload_queue.empty() && _badge_preload_timer != nullptr) {
        lv_timer_delete(_badge_preload_timer);
        _badge_preload_timer = nullptr;
    }
}

void start_badge_preload_timer()
{
    if (_badge_preload_queue.empty()) {
        return;
    }

    if (_badge_preload_timer == nullptr) {
        _badge_preload_timer = lv_timer_create(badge_preload_timer_cb, 80, nullptr);
    } else {
        lv_timer_reset(_badge_preload_timer);
        lv_timer_resume(_badge_preload_timer);
    }
}

void queue_badge_preload_path(const std::string& fs_path)
{
    if (fs_path.empty() || find_decoded_badge_image(fs_path) != nullptr || badge_preload_queue_contains(fs_path)) {
        return;
    }

    _badge_preload_queue.push_back(fs_path);
}

void queue_badge_preload_for_slot(std::size_t slot)
{
    const std::string extension = read_slot_extension(slot);
    if (extension == "jpg" || extension == "png") {
        queue_badge_preload_path(slot_image_path(slot, extension));
    } else if (extension == "banim") {
        uint16_t frame_count = 0;
        uint16_t delay_ms    = 0;
        uint16_t width       = 0;
        uint16_t height      = 0;
        if (read_animation_info(slot, frame_count, delay_ms, width, height)) {
            const std::size_t cache_frame_limit =
                std::max<std::size_t>(1, _decoded_badge_cache_max_bytes / decoded_badge_frame_bytes());
            for (std::size_t index = 0; index < frame_count && index < cache_frame_limit; ++index) {
                queue_badge_preload_path(slot_frame_path(slot, index));
            }
        }
    }

    start_badge_preload_timer();
}

void queue_badge_neighbor_preload(std::size_t current_slot)
{
    const std::size_t next_slot = find_displayable_slot(current_slot, 1);
    if (next_slot < _max_badge_slot_count) {
        queue_badge_preload_for_slot(next_slot);
    }

    const std::size_t previous_slot = find_displayable_slot(current_slot, -1);
    if (previous_slot < _max_badge_slot_count && previous_slot != next_slot) {
        queue_badge_preload_for_slot(previous_slot);
    }
}

void stop_badge_preload()
{
    _badge_preload_queue.clear();
    if (_badge_preload_timer != nullptr) {
        lv_timer_delete(_badge_preload_timer);
        _badge_preload_timer = nullptr;
    }
}

void show_image(lv_obj_t* image)
{
    if (image != nullptr) {
        lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
    }
}

void close_gif(lv_obj_t* gif)
{
    if (gif != nullptr) {
        lv_obj_add_flag(gif, LV_OBJ_FLAG_HIDDEN);
    }
}

void stop_badge_animation()
{
    if (_animation_state.timer != nullptr) {
        lv_timer_delete(_animation_state.timer);
        _animation_state.timer = nullptr;
    }
    stop_badge_preload();

    _animation_state.image = nullptr;
    _animation_state.frame_paths.clear();
    _animation_state.frame_index = 0;
    _animation_state.delay_ms    = 125;
}

void badge_animation_timer_cb(lv_timer_t*)
{
    if (_animation_state.image == nullptr || _animation_state.frame_paths.empty()) {
        stop_badge_animation();
        return;
    }

    _animation_state.frame_index = (_animation_state.frame_index + 1) % _animation_state.frame_paths.size();
    const std::string& path      = _animation_state.frame_paths[_animation_state.frame_index];
    set_badge_image_source(_animation_state.image, path);
    show_image(_animation_state.image);

    const std::size_t next_index = (_animation_state.frame_index + 1) % _animation_state.frame_paths.size();
    queue_badge_preload_path(_animation_state.frame_paths[next_index]);
    start_badge_preload_timer();
}

bool start_badge_animation(lv_obj_t* image, std::size_t slot)
{
    if (image == nullptr) {
        return false;
    }

    uint16_t frame_count = 0;
    uint16_t delay_ms    = 0;
    uint16_t width       = 0;
    uint16_t height      = 0;
    if (!read_animation_info(slot, frame_count, delay_ms, width, height)) {
        mclog::tagWarn(_tag, "invalid badge animation metadata, slot={}", slot);
        return false;
    }

    std::vector<std::string> frame_paths;
    frame_paths.reserve(frame_count);
    for (std::size_t index = 0; index < frame_count; ++index) {
        const std::string fs_path = slot_frame_path(slot, index);
        if (access(fs_path.c_str(), R_OK) != 0) {
            mclog::tagWarn(_tag, "missing badge animation frame: {}", fs_path);
            return false;
        }
        frame_paths.push_back(fs_path);
    }

    stop_badge_animation();

    _animation_state.image       = image;
    _animation_state.frame_paths = std::move(frame_paths);
    _animation_state.frame_index = 0;
    _animation_state.delay_ms    = delay_ms;

    set_badge_image_source(image, _animation_state.frame_paths.front());
    show_image(image);
    _animation_state.timer = lv_timer_create(badge_animation_timer_cb, delay_ms, nullptr);
    if (_animation_state.timer == nullptr) {
        stop_badge_animation();
        return false;
    }

    for (std::size_t index = 1; index < _animation_state.frame_paths.size(); ++index) {
        queue_badge_preload_path(_animation_state.frame_paths[index]);
    }
    start_badge_preload_timer();

    mclog::tagInfo(_tag, "load badge animation, slot={}, frames={}, delay={}ms, size={}x{}", slot, frame_count, delay_ms,
                   width, height);
    return true;
}

bool load_badge_slot(const Hal::BadgeRenderTarget& target, std::size_t slot)
{
    if ((target.image == nullptr && target.gif == nullptr) || slot >= _max_badge_slot_count) {
        return false;
    }

    const std::string extension = read_slot_extension(slot);
    if (extension.empty()) {
        return false;
    }

    const std::string fs_path = slot_image_path(slot, extension);
    if (access(fs_path.c_str(), R_OK) != 0) {
        return false;
    }

    if (extension == "gif") {
        mclog::tagWarn(_tag, "gif badge display is disabled to avoid decoder reset: {}", fs_path);
        close_gif(target.gif);
        return false;
    }

    if (extension == "banim") {
        close_gif(target.gif);
        if (start_badge_animation(target.image, slot)) {
            persist_active_slot(slot);
            queue_badge_neighbor_preload(slot);
            return true;
        }
        return false;
    }

    if (target.image == nullptr) {
        return false;
    }

    stop_badge_animation();
    close_gif(target.gif);
    set_badge_image_source(target.image, fs_path);
    show_image(target.image);
    persist_active_slot(slot);
    queue_badge_neighbor_preload(slot);
    mclog::tagInfo(_tag, "load badge image from {}, slot={}", fs_path, slot);
    return true;
}

bool load_badge_slot(lv_obj_t* image, std::size_t slot)
{
    return load_badge_slot({image, nullptr}, slot);
}

void show_badge_placeholder(const Hal::BadgeRenderTarget& target)
{
    stop_badge_animation();
    close_gif(target.gif);
    _visible_decoded_badge_image = nullptr;
    if (target.image != nullptr) {
        lv_image_set_src(target.image, &icon_badge);
        show_image(target.image);
    }
}

badge::config_ap::BadgeState make_badge_state()
{
    badge::config_ap::BadgeState state;
    state.slotCount  = _max_badge_slot_count;
    state.activeSlot = read_active_slot();
    state.slots.reserve(_max_badge_slot_count);

    for (std::size_t slot = 0; slot < _max_badge_slot_count; ++slot) {
        state.slots.push_back({
            .slot     = slot,
            .hasImage = is_slot_available(slot),
            .isActive = slot == state.activeSlot,
        });
    }

    return state;
}

bool get_badge_image(std::size_t slot, badge::config_ap::ImageData& image, std::string& message)
{
    if (slot >= _max_badge_slot_count) {
        message = "invalid badge slot";
        return false;
    }

    const std::string extension = read_slot_extension(slot);
    if (extension.empty()) {
        message = "badge image not found";
        return false;
    }

    const std::string path = extension == "banim" ? slot_frame_path(slot, 0) : slot_image_path(slot, extension);
    if (!read_binary_file(path, image.data)) {
        message = "failed to read image file";
        return false;
    }

    image.contentType = extension == "banim" ? "image/jpeg" : slot_content_type(extension);
    return true;
}

bool set_active_badge_slot(std::size_t slot, std::string& message)
{
    if (slot >= _max_badge_slot_count) {
        message = "invalid badge slot";
        return false;
    }
    if (!is_slot_available(slot)) {
        message = "badge image not found";
        return false;
    }
    if (!is_slot_displayable(slot)) {
        message = "gif badge playback is temporarily disabled";
        return false;
    }
    if (!persist_active_slot(slot)) {
        message = "failed to update active slot";
        return false;
    }

    message = "active slot updated";
    return true;
}

bool delete_badge_slot(std::size_t slot, std::string& message)
{
    if (slot >= _max_badge_slot_count) {
        message = "invalid badge slot";
        return false;
    }

    mark_decoded_badge_cache_stale_for_slot(slot);

    bool deleted = false;
    for (const auto* candidate : {"png", "jpg", "jpeg", "gif", "banim"}) {
        const std::string path = slot_image_path(slot, candidate);
        if (unlink(path.c_str()) == 0) {
            deleted = true;
        }
    }

    for (std::size_t index = 0; index < _max_animation_frames; ++index) {
        const std::string path = slot_frame_path(slot, index);
        if (unlink(path.c_str()) == 0) {
            deleted = true;
        }
    }
    rmdir(slot_frames_dir(slot).c_str());

    const std::string meta_path = slot_meta_path(slot);
    if (unlink(meta_path.c_str()) == 0) {
        deleted = true;
    }

    if (!deleted) {
        message = "badge image not found";
        return false;
    }

    if (read_active_slot() == slot) {
        const std::size_t next_slot = find_first_displayable_slot();
        if (next_slot < _max_badge_slot_count) {
            persist_active_slot(next_slot);
        } else {
            clear_active_slot();
        }
    }

    message = "badge image deleted";
    return true;
}

void delete_slot_animation_files(std::size_t slot)
{
    unlink(slot_image_path(slot, "banim").c_str());
    for (std::size_t index = 0; index < _max_animation_frames; ++index) {
        unlink(slot_frame_path(slot, index).c_str());
    }
    rmdir(slot_frames_dir(slot).c_str());
}

bool save_badge_animation_upload(const badge::config_ap::UploadRequest& request, std::string& message)
{
    constexpr std::size_t header_size = 14;
    if (request.data.size() < header_size ||
        memcmp(request.data.data(), _animation_magic, sizeof(_animation_magic)) != 0) {
        message = "invalid animation package";
        return false;
    }

    const uint16_t width       = read_le16(request.data, 6);
    const uint16_t height      = read_le16(request.data, 8);
    const uint16_t frame_count = read_le16(request.data, 10);
    const uint16_t delay_ms    = read_le16(request.data, 12);
    if (width == 0 || width > _max_animation_size || height == 0 || height > _max_animation_size || frame_count == 0 ||
        frame_count > _max_animation_frames || delay_ms < _min_animation_delay_ms || delay_ms > _max_animation_delay_ms) {
        message = "unsupported animation settings";
        return false;
    }

    if (!ensure_slot_frames_dir(request.slot)) {
        message = "failed to create animation storage";
        return false;
    }

    std::size_t offset = header_size;
    std::vector<std::vector<uint8_t>> frames;
    frames.reserve(frame_count);
    for (std::size_t index = 0; index < frame_count; ++index) {
        if (offset + 4 > request.data.size()) {
            message = "truncated animation package";
            return false;
        }

        const uint32_t frame_size = read_le32(request.data, offset);
        offset += 4;
        if (frame_size == 0 || offset + frame_size > request.data.size() ||
            !is_jpeg_data(request.data, offset, frame_size)) {
            message = "invalid animation frame";
            return false;
        }

        frames.emplace_back(request.data.begin() + static_cast<std::ptrdiff_t>(offset),
                            request.data.begin() + static_cast<std::ptrdiff_t>(offset + frame_size));
        offset += frame_size;
    }

    if (offset != request.data.size()) {
        message = "invalid animation package size";
        return false;
    }

    mark_decoded_badge_cache_stale_for_slot(request.slot);

    for (const auto* candidate : {"png", "jpg", "jpeg", "gif"}) {
        unlink(slot_image_path(request.slot, candidate).c_str());
    }
    delete_slot_animation_files(request.slot);
    if (!ensure_slot_frames_dir(request.slot)) {
        message = "failed to create animation storage";
        return false;
    }

    for (std::size_t index = 0; index < frames.size(); ++index) {
        if (!write_binary_file(slot_frame_path(request.slot, index), frames[index])) {
            message = "failed to store animation frame";
            return false;
        }
    }

    char info[32] = {};
    snprintf(info, sizeof(info), "%u %u %u %u", static_cast<unsigned>(frame_count), static_cast<unsigned>(delay_ms),
             static_cast<unsigned>(width), static_cast<unsigned>(height));
    if (!write_text_file(slot_image_path(request.slot, "banim"), info)) {
        message = "failed to store animation metadata";
        return false;
    }
    if (!write_text_file(slot_meta_path(request.slot), "banim")) {
        message = "failed to store image metadata";
        return false;
    }
    if (!write_text_file(_active_slot_file, std::to_string(request.slot))) {
        message = "failed to store active slot";
        return false;
    }

    mclog::tagInfo(_tag, "badge animation saved, slot={}, frames={}, delay={}ms", request.slot, frames.size(), delay_ms);
    message = "animation upload success";
    return true;
}

bool save_badge_upload(const badge::config_ap::UploadRequest& request, std::string& message)
{
    if (request.slot >= _max_badge_slot_count) {
        message = "invalid badge slot";
        return false;
    }
    if (request.data.empty()) {
        message = "empty upload body";
        return false;
    }
    if (!ensure_badge_dir()) {
        message = "failed to create badge directory";
        return false;
    }

    const std::string extension = normalize_extension(request.fileName, request.contentType);
    if (extension == "banim") {
        return save_badge_animation_upload(request, message);
    }

    if (extension != "jpg") {
        message = "only jpg images are supported";
        return false;
    }

    const std::string final_path = slot_image_path(request.slot, extension);
    const std::string temp_path  = final_path + ".tmp";
    if (!write_binary_file(temp_path, request.data)) {
        message = "failed to store image";
        return false;
    }

    mark_decoded_badge_cache_stale_for_slot(request.slot);

    if (unlink(final_path.c_str()) != 0 && errno != ENOENT) {
        unlink(temp_path.c_str());
        message = "failed to replace existing image";
        return false;
    }

    if (rename(temp_path.c_str(), final_path.c_str()) != 0) {
        unlink(temp_path.c_str());
        message = "failed to finalize image";
        return false;
    }

    for (const auto* candidate : {"png", "jpg", "jpeg", "gif", "banim"}) {
        if (extension == candidate || (extension == "jpg" && strcmp(candidate, "jpeg") == 0)) {
            continue;
        }
        const std::string old_path = slot_image_path(request.slot, candidate);
        unlink(old_path.c_str());
    }
    delete_slot_animation_files(request.slot);

    if (!write_text_file(slot_meta_path(request.slot), extension)) {
        message = "failed to store image metadata";
        return false;
    }
    if (!write_text_file(_active_slot_file, std::to_string(request.slot))) {
        message = "failed to store active slot";
        return false;
    }

    mclog::tagInfo(_tag, "badge image saved, slot={}, path={}", request.slot, final_path);
    message = "upload success";
    return true;
}

}  // namespace

bool Hal::loadBadgeImage(const BadgeRenderTarget& target)
{
    if (target.image == nullptr && target.gif == nullptr) {
        return false;
    }

    const std::size_t active_slot = read_active_slot();
    if (load_badge_slot(target, active_slot)) {
        return true;
    }

    const std::size_t fallback_slot = find_first_displayable_slot();
    if (fallback_slot < _max_badge_slot_count && load_badge_slot(target, fallback_slot)) {
        return true;
    }

    show_badge_placeholder(target);
    mclog::tagWarn(_tag, "badge image not found, use default asset");
    return false;
}

bool Hal::loadNextBadgeImage(const BadgeRenderTarget& target)
{
    mclog::tagInfo(_tag, "load next badge image");

    const std::size_t slot = find_displayable_slot(read_active_slot(), 1);
    if (slot < _max_badge_slot_count && load_badge_slot(target, slot)) {
        return true;
    }

    show_badge_placeholder(target);
    return false;
}

bool Hal::loadPreviousBadgeImage(const BadgeRenderTarget& target)
{
    mclog::tagInfo(_tag, "load previous badge image");

    const std::size_t slot = find_displayable_slot(read_active_slot(), -1);
    if (slot < _max_badge_slot_count && load_badge_slot(target, slot)) {
        return true;
    }

    show_badge_placeholder(target);
    return false;
}

bool Hal::loadBadgeImage(lv_obj_t* image)
{
    return loadBadgeImage({image, nullptr});
}

bool Hal::loadNextBadgeImage(lv_obj_t* image)
{
    return loadNextBadgeImage({image, nullptr});
}

bool Hal::loadPreviousBadgeImage(lv_obj_t* image)
{
    return loadPreviousBadgeImage({image, nullptr});
}

void Hal::stopBadgeAnimation()
{
    stop_badge_animation();
}

void Hal::startBadgeEditModeViaAp(std::function<void(std::string_view)> onLog)
{
    mclog::tagInfo(_tag, "start badge edit mode via AP");

    if (!ensure_badge_dir()) {
        if (onLog) {
            onLog("Failed to create badge storage directory");
        }
        return;
    }

    badge::config_ap::run(onLog, {
                                     .onUpload    = save_badge_upload,
                                     .onGetState  = make_badge_state,
                                     .onGetImage  = get_badge_image,
                                     .onSetActive = set_active_badge_slot,
                                     .onDelete    = delete_badge_slot,
                                 });
}
