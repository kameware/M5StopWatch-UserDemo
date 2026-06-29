/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "view.h"

#include <assets/assets.h>
#include <hal/hal.h>
#include <mooncake_log.h>
#include <array>
#include <cmath>

using namespace view;

namespace {

constexpr int _panel_size          = 466;
constexpr int _panel_center        = _panel_size / 2;
constexpr int _avatar_image_size   = 251;
constexpr int _center_dead_zone_px = 50;
constexpr float _rad_to_deg        = 57.29577951308232f;
constexpr const char* _tag         = "Guruguru";

const std::array<const lv_image_dsc_t*, 9> _direction_images = {
    &guruguru_avatar_dir0,
    &guruguru_avatar_dir1,
    &guruguru_avatar_dir2,
    &guruguru_avatar_dir3,
    &guruguru_avatar_dir4,
    &guruguru_avatar_dir5,
    &guruguru_avatar_dir6,
    &guruguru_avatar_dir7,
    &guruguru_avatar_dir8,
};

int direction_from_point(int x, int y)
{
    const float dx = static_cast<float>(x - _panel_center);
    const float dy = static_cast<float>(y - _panel_center);

    if (std::sqrt((dx * dx) + (dy * dy)) < _center_dead_zone_px) {
        return 4;
    }

    const float angle = std::atan2(dy, dx) * _rad_to_deg;

    if (angle < -157.5f || angle > 157.5f) {
        return 3;
    } else if (angle < -112.5f) {
        return 0;
    } else if (angle < -67.5f) {
        return 1;
    } else if (angle < -22.5f) {
        return 2;
    } else if (angle < 22.5f) {
        return 5;
    } else if (angle < 67.5f) {
        return 8;
    } else if (angle < 112.5f) {
        return 7;
    } else if (angle < 157.5f) {
        return 6;
    }
    return 3;
}

}  // namespace

void GuruguruView::init()
{
    auto& display = GetHAL().getDisplay();
    display.fillScreen(TFT_BLACK);

    if (!initBuffers()) {
        return;
    }

    setDirection(4);
}

void GuruguruView::trackFace(int x, int y)
{
    setDirection(direction_from_point(x, y));
}

bool GuruguruView::initBuffers()
{
    auto& display = GetHAL().getDisplay();

    _source_canvas = std::make_unique<M5Canvas>(&display);
    _source_canvas->setColorDepth(16);
    _source_canvas->setPsram(true);
    if (_source_canvas->createSprite(_avatar_image_size, _avatar_image_size) == nullptr) {
        mclog::tagWarn(_tag, "failed to allocate source sprite");
        _source_canvas.reset();
        return false;
    }

    _composite = std::make_unique<M5Canvas>(&display);
    _composite->setColorDepth(16);
    _composite->setPsram(true);
    if (_composite->createSprite(display.width(), display.height()) == nullptr) {
        mclog::tagWarn(_tag, "failed to allocate composite sprite");
        _source_canvas.reset();
        _composite.reset();
        return false;
    }

    return true;
}

bool GuruguruView::loadDirection(int direction)
{
    if (direction < 0 || direction >= static_cast<int>(_direction_images.size())) {
        return false;
    }

    if (!_source_canvas) {
        return false;
    }

    if (direction == _loaded_direction) {
        return true;
    }

    _source_canvas->fillSprite(TFT_BLACK);
    const auto* image = _direction_images[direction];
    if (!_source_canvas->drawPng(image->data, static_cast<uint32_t>(image->data_size), 0, 0)) {
        mclog::tagWarn(_tag, "failed to decode direction image: {}", direction);
        _loaded_direction = -1;
        return false;
    }

    _loaded_direction = direction;
    return true;
}

void GuruguruView::render()
{
    if (!_composite || !_source_canvas) {
        return;
    }

    if (!loadDirection(_current_direction)) {
        return;
    }

    auto& display = GetHAL().getDisplay();
    const float zoom_x = static_cast<float>(display.width()) / _avatar_image_size;
    const float zoom_y = static_cast<float>(display.height()) / _avatar_image_size;

    _composite->fillSprite(TFT_BLACK);
    _source_canvas->pushRotateZoom(_composite.get(), display.width() / 2, display.height() / 2, 0.0f, zoom_x, zoom_y);

    display.startWrite();
    _composite->pushSprite(&display, 0, 0);
    display.endWrite();
}

void GuruguruView::setDirection(int direction)
{
    if (direction < 0 || direction >= static_cast<int>(_direction_images.size())) {
        return;
    }

    if (direction == _current_direction && direction == _loaded_direction) {
        return;
    }

    _current_direction = direction;
    render();
}
