/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "phone_notifications.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sdkconfig.h>

#include <cJSON.h>
#include <esp_log.h>

namespace {

static constexpr const char* TAG         = "PhoneNotifications";
static constexpr std::size_t MAX_HISTORY = 8;

SemaphoreHandle_t g_lock = nullptr;
std::vector<PhoneNotification> g_notifications;
uint32_t g_sequence       = 0;
bool g_ble_ready          = false;
bool g_connected          = false;
bool g_ancs_ready         = false;
bool g_init_started       = false;

void ensure_lock()
{
    if (g_lock == nullptr) {
        g_lock = xSemaphoreCreateMutex();
    }
}

class ScopedNotificationLock {
public:
    ScopedNotificationLock()
    {
        ensure_lock();
        _locked = g_lock != nullptr && xSemaphoreTake(g_lock, pdMS_TO_TICKS(50)) == pdTRUE;
    }

    ~ScopedNotificationLock()
    {
        if (_locked) {
            xSemaphoreGive(g_lock);
        }
    }

    bool locked() const
    {
        return _locked;
    }

private:
    bool _locked = false;
};

std::string timestamp_now()
{
    std::time_t now = std::time(nullptr);
    if (now < 1600000000) {
        return "--/-- --:--";
    }

    std::tm local_tm {};
    localtime_r(&now, &local_tm);

    char buffer[20] {};
    if (std::strftime(buffer, sizeof(buffer), "%m/%d %H:%M", &local_tm) == 0) {
        return "--/-- --:--";
    }
    return buffer;
}

std::string compact_text(std::string value, std::size_t max_bytes, const char* fallback)
{
    std::replace(value.begin(), value.end(), '\r', ' ');
    std::replace(value.begin(), value.end(), '\n', ' ');
    while (!value.empty() && value.front() == ' ') {
        value.erase(value.begin());
    }
    while (!value.empty() && value.back() == ' ') {
        value.pop_back();
    }
    if (value.empty()) {
        value = fallback;
    }
    if (value.size() <= max_bytes) {
        return value;
    }

    std::size_t end = max_bytes;
    while (end > 0 && end < value.size() && (static_cast<unsigned char>(value[end]) & 0xC0) == 0x80) {
        --end;
    }
    if (end == 0) {
        end = max_bytes;
    }
    value.resize(end);
    value += "...";
    return value;
}

std::string get_json_string(cJSON* root, const char* key)
{
    auto* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        return item->valuestring;
    }
    return {};
}

void add_notification(PhoneNotification notification)
{
    ScopedNotificationLock lock;
    if (!lock.locked()) {
        ESP_LOGW(TAG, "notification lock timeout");
        return;
    }

    notification.sequence   = ++g_sequence;
    notification.receivedAt = timestamp_now();

    g_notifications.insert(g_notifications.begin(), std::move(notification));
    if (g_notifications.size() > MAX_HISTORY) {
        g_notifications.resize(MAX_HISTORY);
    }
}

void set_connected(bool connected)
{
    ScopedNotificationLock lock;
    if (lock.locked()) {
        g_connected = connected;
    }
}

void set_ancs_ready(bool ready)
{
    ScopedNotificationLock lock;
    if (lock.locked()) {
        g_ancs_ready = ready;
    }
}

PhoneNotification parse_payload(const std::string& payload)
{
    PhoneNotification notification;
    notification.app   = "Phone";
    notification.title = "Notification";
    notification.body  = payload;

    cJSON* root = cJSON_ParseWithLength(payload.data(), payload.size());
    if (root != nullptr) {
        if (cJSON_IsObject(root)) {
            notification.app   = get_json_string(root, "app");
            notification.title = get_json_string(root, "title");
            notification.body  = get_json_string(root, "body");
        }
        cJSON_Delete(root);
    }

    notification.app   = compact_text(notification.app, 32, "Phone");
    notification.title = compact_text(notification.title, 72, "Notification");
    notification.body  = compact_text(notification.body, 220, "");
    return notification;
}

}  // namespace

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED

#include <host/ble_hs.h>
#include <host/ble_hs_mbuf.h>
#include <host/ble_store.h>
#include <host/ble_sm.h>
#include <host/ble_uuid.h>
#include <host/util/util.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <os/os_mbuf.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>
#include <store/config/ble_store_config.h>

extern "C" void ble_store_config_init(void);

namespace {

uint8_t g_own_addr_type = 0;
uint16_t g_mtu_size     = 23;

enum AncsEventId : uint8_t {
    ANCS_EVENT_ADDED    = 0,
    ANCS_EVENT_MODIFIED = 1,
    ANCS_EVENT_REMOVED  = 2,
};

enum AncsCommandId : uint8_t {
    ANCS_COMMAND_GET_NOTIFICATION_ATTRIBUTES = 0,
};

enum AncsAttributeId : uint8_t {
    ANCS_ATTR_APP_IDENTIFIER = 0,
    ANCS_ATTR_TITLE          = 1,
    ANCS_ATTR_SUBTITLE       = 2,
    ANCS_ATTR_MESSAGE        = 3,
    ANCS_ATTR_DATE           = 5,
};

const ble_uuid128_t k_ancs_service_uuid =
    BLE_UUID128_INIT(0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4, 0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79);
const ble_uuid128_t k_ancs_notification_source_uuid =
    BLE_UUID128_INIT(0xBD, 0x1D, 0xA2, 0x99, 0xE6, 0x25, 0x58, 0x8C, 0xD9, 0x42, 0x01, 0x63, 0x0D, 0x12, 0xBF, 0x9F);
const ble_uuid128_t k_ancs_control_point_uuid =
    BLE_UUID128_INIT(0xD9, 0xD9, 0xAA, 0xFD, 0xBD, 0x9B, 0x21, 0x98, 0xA8, 0x49, 0xE1, 0x45, 0xF3, 0xD8, 0xD1, 0x69);
const ble_uuid128_t k_ancs_data_source_uuid =
    BLE_UUID128_INIT(0xFB, 0x7B, 0x7C, 0xCE, 0x6A, 0xB3, 0x44, 0xBE, 0xB5, 0x4B, 0xD6, 0x24, 0xE9, 0xC6, 0xEA, 0x22);
const ble_uuid16_t k_cccd_uuid = BLE_UUID16_INIT(BLE_GATT_DSC_CLT_CFG_UUID16);

const ble_uuid128_t k_service_uuid =
    BLE_UUID128_INIT(0x00, 0x57, 0x54, 0x53, 0x4D, 0x53, 0x65, 0x9D, 0x35, 0x4D, 0x6F, 0x6D, 0x01, 0x00, 0x3F, 0x7B);
const ble_uuid128_t k_write_uuid =
    BLE_UUID128_INIT(0x00, 0x57, 0x54, 0x53, 0x4D, 0x53, 0x65, 0x9D, 0x35, 0x4D, 0x6F, 0x6D, 0x02, 0x00, 0x3F, 0x7B);
const ble_uuid128_t k_status_uuid =
    BLE_UUID128_INIT(0x00, 0x57, 0x54, 0x53, 0x4D, 0x53, 0x65, 0x9D, 0x35, 0x4D, 0x6F, 0x6D, 0x03, 0x00, 0x3F, 0x7B);

int notification_gatt_access(uint16_t conn_handle, uint16_t attr_handle, ble_gatt_access_ctxt* ctxt, void* arg);

ble_gatt_chr_def k_characteristics[] = {
    {&k_write_uuid.u, notification_gatt_access, nullptr, nullptr, BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP, 0,
     nullptr, nullptr},
    {&k_status_uuid.u, notification_gatt_access, nullptr, nullptr, BLE_GATT_CHR_F_READ, 0, nullptr, nullptr},
    {nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, nullptr},
};

ble_gatt_svc_def k_services[] = {
    {BLE_GATT_SVC_TYPE_PRIMARY, &k_service_uuid.u, nullptr, k_characteristics},
    {0, nullptr, nullptr, nullptr},
};

struct AncsState {
    uint16_t conn_handle                 = BLE_HS_CONN_HANDLE_NONE;
    uint16_t service_end_handle          = 0;
    uint16_t notification_source_def_handle = 0;
    uint16_t notification_source_handle  = 0;
    uint16_t notification_source_cccd_handle = 0;
    uint16_t control_point_def_handle    = 0;
    uint16_t control_point_handle        = 0;
    uint16_t data_source_def_handle      = 0;
    uint16_t data_source_handle          = 0;
    uint16_t data_source_cccd_handle     = 0;
    bool notification_source_descriptor_done = false;
    bool notification_source_subscribed  = false;
    bool data_source_descriptor_done     = false;
    bool data_source_subscribed          = false;
};

AncsState g_ancs;
SemaphoreHandle_t g_ancs_buffer_lock = nullptr;
esp_timer_handle_t g_ancs_flush_timer = nullptr;
std::vector<uint8_t> g_ancs_data_buffer;

void ensure_ancs_buffer_lock()
{
    if (g_ancs_buffer_lock == nullptr) {
        g_ancs_buffer_lock = xSemaphoreCreateMutex();
    }
}

class ScopedAncsBufferLock {
public:
    ScopedAncsBufferLock()
    {
        ensure_ancs_buffer_lock();
        _locked = g_ancs_buffer_lock != nullptr && xSemaphoreTake(g_ancs_buffer_lock, pdMS_TO_TICKS(50)) == pdTRUE;
    }

    ~ScopedAncsBufferLock()
    {
        if (_locked) {
            xSemaphoreGive(g_ancs_buffer_lock);
        }
    }

    bool locked() const
    {
        return _locked;
    }

private:
    bool _locked = false;
};

void reset_ancs_state(uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE)
{
    g_ancs = {};
    g_ancs.conn_handle = conn_handle;
    set_ancs_ready(false);
}

std::string build_status_payload()
{
    ScopedNotificationLock lock;
    uint32_t count    = lock.locked() ? g_notifications.size() : 0;
    uint32_t sequence = lock.locked() ? g_sequence : 0;
    bool connected    = lock.locked() ? g_connected : false;

    char buffer[96] {};
    std::snprintf(buffer, sizeof(buffer), "{\"ready\":true,\"connected\":%s,\"count\":%lu,\"sequence\":%lu}",
                  connected ? "true" : "false", static_cast<unsigned long>(count), static_cast<unsigned long>(sequence));
    return buffer;
}

int notification_gatt_access(uint16_t conn_handle, uint16_t attr_handle, ble_gatt_access_ctxt* ctxt, void* arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    const ble_uuid_t* uuid = ctxt->chr->uuid;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && ble_uuid_cmp(uuid, &k_write_uuid.u) == 0) {
        uint16_t payload_len = OS_MBUF_PKTLEN(ctxt->om);
        if (payload_len == 0 || payload_len > 512) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        std::string payload(payload_len, '\0');
        uint16_t copied = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, payload.data(), payload_len, &copied);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        payload.resize(copied);

        phone_notifications_receive_payload(payload);
        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && ble_uuid_cmp(uuid, &k_status_uuid.u) == 0) {
        std::string status = build_status_payload();
        int rc             = os_mbuf_append(ctxt->om, status.data(), status.size());
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

#if CONFIG_BT_NIMBLE_GATT_CLIENT

enum AncsSubscribeTarget : uint8_t {
    ANCS_SUBSCRIBE_NONE = 0,
    ANCS_SUBSCRIBE_NOTIFICATION_SOURCE = 1,
    ANCS_SUBSCRIBE_DATA_SOURCE = 2,
};

std::string ancs_string(const uint8_t* data, std::size_t len)
{
    if (data == nullptr || len == 0) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(data), len);
}

void handle_ancs_attributes(const std::vector<uint8_t>& data)
{
    if (data.size() < 5 || data[0] != ANCS_COMMAND_GET_NOTIFICATION_ATTRIBUTES) {
        return;
    }

    std::string app;
    std::string title;
    std::string subtitle;
    std::string message;
    std::string date;

    std::size_t index = 5;
    while (index + 3 <= data.size()) {
        uint8_t attr_id  = data[index++];
        uint16_t attr_len = data[index] | (data[index + 1] << 8);
        index += 2;
        if (index + attr_len > data.size()) {
            ESP_LOGW(TAG, "truncated ANCS attribute payload");
            break;
        }

        std::string value = ancs_string(&data[index], attr_len);
        switch (attr_id) {
            case ANCS_ATTR_APP_IDENTIFIER:
                app = std::move(value);
                break;
            case ANCS_ATTR_TITLE:
                title = std::move(value);
                break;
            case ANCS_ATTR_SUBTITLE:
                subtitle = std::move(value);
                break;
            case ANCS_ATTR_MESSAGE:
                message = std::move(value);
                break;
            case ANCS_ATTR_DATE:
                date = std::move(value);
                break;
            default:
                break;
        }
        index += attr_len;
    }

    if (app.empty() && title.empty() && subtitle.empty() && message.empty()) {
        return;
    }

    PhoneNotification notification;
    notification.app = compact_text(app, 32, "iPhone");
    notification.title = compact_text(title.empty() ? subtitle : title, 72, "iPhone Notification");
    if (!subtitle.empty() && !message.empty() && subtitle != message) {
        notification.body = compact_text(subtitle + "  " + message, 220, "");
    } else {
        notification.body = compact_text(message.empty() ? date : message, 220, "");
    }
    add_notification(std::move(notification));
}

void process_ancs_data_buffer()
{
    std::vector<uint8_t> payload;
    {
        ScopedAncsBufferLock lock;
        if (!lock.locked() || g_ancs_data_buffer.empty()) {
            return;
        }
        payload.swap(g_ancs_data_buffer);
    }
    handle_ancs_attributes(payload);
}

void ancs_flush_timer_callback(void* arg)
{
    (void)arg;
    process_ancs_data_buffer();
}

void schedule_ancs_data_flush()
{
    if (g_ancs_flush_timer == nullptr) {
        return;
    }
    esp_timer_stop(g_ancs_flush_timer);
    esp_timer_start_once(g_ancs_flush_timer, 250000);
}

void append_ancs_data_source(os_mbuf* om)
{
    uint16_t len = OS_MBUF_PKTLEN(om);
    if (len == 0) {
        return;
    }

    std::vector<uint8_t> chunk(len);
    uint16_t copied = 0;
    int rc = ble_hs_mbuf_to_flat(om, chunk.data(), len, &copied);
    if (rc != 0 || copied == 0) {
        ESP_LOGW(TAG, "failed to flatten ANCS data source: %d", rc);
        return;
    }
    chunk.resize(copied);

    bool flush_now = copied < (g_mtu_size > 3 ? g_mtu_size - 3 : g_mtu_size);
    {
        ScopedAncsBufferLock lock;
        if (!lock.locked()) {
            return;
        }
        if (g_ancs_data_buffer.size() + chunk.size() > 1024) {
            ESP_LOGW(TAG, "ANCS data buffer overflow");
            g_ancs_data_buffer.clear();
        }
        g_ancs_data_buffer.insert(g_ancs_data_buffer.end(), chunk.begin(), chunk.end());
    }

    if (flush_now) {
        if (g_ancs_flush_timer != nullptr) {
            esp_timer_stop(g_ancs_flush_timer);
        }
        process_ancs_data_buffer();
    } else {
        schedule_ancs_data_flush();
    }
}

int ancs_write_cb(uint16_t conn_handle, const ble_gatt_error* error, ble_gatt_attr* attr, void* arg)
{
    (void)conn_handle;
    (void)attr;
    const char* label = static_cast<const char*>(arg);
    if (error->status == 0) {
        ESP_LOGI(TAG, "ANCS %s write complete", label ? label : "characteristic");
    } else {
        ESP_LOGW(TAG, "ANCS %s write failed: %d", label ? label : "characteristic", error->status);
    }
    return 0;
}

void discover_next_ancs_cccd(uint16_t conn_handle);

const char* ancs_target_label(AncsSubscribeTarget target)
{
    switch (target) {
        case ANCS_SUBSCRIBE_NOTIFICATION_SOURCE:
            return "Notification Source";
        case ANCS_SUBSCRIBE_DATA_SOURCE:
            return "Data Source";
        default:
            return "unknown";
    }
}

void* ancs_target_arg(AncsSubscribeTarget target)
{
    return reinterpret_cast<void*>(static_cast<uintptr_t>(target));
}

AncsSubscribeTarget ancs_target_from_arg(void* arg)
{
    return static_cast<AncsSubscribeTarget>(reinterpret_cast<uintptr_t>(arg));
}

uint16_t ancs_target_value_handle(AncsSubscribeTarget target)
{
    switch (target) {
        case ANCS_SUBSCRIBE_NOTIFICATION_SOURCE:
            return g_ancs.notification_source_handle;
        case ANCS_SUBSCRIBE_DATA_SOURCE:
            return g_ancs.data_source_handle;
        default:
            return 0;
    }
}

uint16_t ancs_target_def_handle(AncsSubscribeTarget target)
{
    switch (target) {
        case ANCS_SUBSCRIBE_NOTIFICATION_SOURCE:
            return g_ancs.notification_source_def_handle;
        case ANCS_SUBSCRIBE_DATA_SOURCE:
            return g_ancs.data_source_def_handle;
        default:
            return 0;
    }
}

uint16_t ancs_target_cccd_handle(AncsSubscribeTarget target)
{
    switch (target) {
        case ANCS_SUBSCRIBE_NOTIFICATION_SOURCE:
            return g_ancs.notification_source_cccd_handle;
        case ANCS_SUBSCRIBE_DATA_SOURCE:
            return g_ancs.data_source_cccd_handle;
        default:
            return 0;
    }
}

void set_ancs_target_cccd_handle(AncsSubscribeTarget target, uint16_t handle)
{
    if (target == ANCS_SUBSCRIBE_NOTIFICATION_SOURCE) {
        g_ancs.notification_source_cccd_handle = handle;
    } else if (target == ANCS_SUBSCRIBE_DATA_SOURCE) {
        g_ancs.data_source_cccd_handle = handle;
    }
}

void mark_ancs_target_descriptor_done(AncsSubscribeTarget target)
{
    if (target == ANCS_SUBSCRIBE_NOTIFICATION_SOURCE) {
        g_ancs.notification_source_descriptor_done = true;
    } else if (target == ANCS_SUBSCRIBE_DATA_SOURCE) {
        g_ancs.data_source_descriptor_done = true;
    }
}

bool ancs_target_descriptor_done(AncsSubscribeTarget target)
{
    if (target == ANCS_SUBSCRIBE_NOTIFICATION_SOURCE) {
        return g_ancs.notification_source_descriptor_done;
    }
    if (target == ANCS_SUBSCRIBE_DATA_SOURCE) {
        return g_ancs.data_source_descriptor_done;
    }
    return true;
}

void mark_ancs_target_subscribed(AncsSubscribeTarget target, bool subscribed)
{
    if (target == ANCS_SUBSCRIBE_NOTIFICATION_SOURCE) {
        g_ancs.notification_source_subscribed = subscribed;
    } else if (target == ANCS_SUBSCRIBE_DATA_SOURCE) {
        g_ancs.data_source_subscribed = subscribed;
    }
}

uint16_t ancs_descriptor_end_handle(AncsSubscribeTarget target)
{
    uint16_t def_handle = ancs_target_def_handle(target);
    if (def_handle == 0) {
        return 0;
    }

    uint16_t end = g_ancs.service_end_handle;
    const uint16_t candidate_def_handles[] = {
        g_ancs.notification_source_def_handle,
        g_ancs.control_point_def_handle,
        g_ancs.data_source_def_handle,
    };
    for (uint16_t candidate : candidate_def_handles) {
        if (candidate > def_handle && candidate - 1 < end) {
            end = candidate - 1;
        }
    }
    return end;
}

void update_ancs_ready()
{
    bool ready = g_ancs.notification_source_subscribed && g_ancs.data_source_subscribed && g_ancs.control_point_handle != 0;
    set_ancs_ready(ready);
    if (ready) {
        ESP_LOGI(TAG, "iPhone ANCS ready");
    }
}

int ancs_subscribe_write_cb(uint16_t conn_handle, const ble_gatt_error* error, ble_gatt_attr* attr, void* arg)
{
    (void)attr;
    AncsSubscribeTarget target = ancs_target_from_arg(arg);
    if (conn_handle != g_ancs.conn_handle) {
        return 0;
    }

    const char* label = ancs_target_label(target);
    if (error->status == 0) {
        mark_ancs_target_subscribed(target, true);
        ESP_LOGI(TAG, "ANCS %s subscribed", label);
    } else {
        mark_ancs_target_subscribed(target, false);
        ESP_LOGW(TAG, "ANCS %s subscribe failed: %d", label, error->status);
    }

    update_ancs_ready();
    discover_next_ancs_cccd(conn_handle);
    return 0;
}

void subscribe_discovered_ancs_cccd(uint16_t conn_handle, AncsSubscribeTarget target)
{
    uint16_t cccd_handle = ancs_target_cccd_handle(target);
    const char* label = ancs_target_label(target);
    if (cccd_handle == 0) {
        ESP_LOGW(TAG, "ANCS %s CCCD not found", label);
        discover_next_ancs_cccd(conn_handle);
        return;
    }

    uint8_t cccd_val[2] = {0x01, 0x00};
    int rc = ble_gattc_write_flat(conn_handle, cccd_handle, cccd_val, sizeof(cccd_val), ancs_subscribe_write_cb,
                                  ancs_target_arg(target));
    if (rc != 0) {
        ESP_LOGW(TAG, "failed to subscribe ANCS %s: %d", label, rc);
        discover_next_ancs_cccd(conn_handle);
    }
}

int ancs_dsc_discovered_cb(uint16_t conn_handle, const ble_gatt_error* error, uint16_t chr_val_handle,
                           const ble_gatt_dsc* dsc, void* arg)
{
    AncsSubscribeTarget target = ancs_target_from_arg(arg);
    if (conn_handle != g_ancs.conn_handle || chr_val_handle != ancs_target_value_handle(target)) {
        return 0;
    }

    if (error->status == 0 && dsc != nullptr) {
        if (ble_uuid_cmp(&dsc->uuid.u, &k_cccd_uuid.u) == 0) {
            set_ancs_target_cccd_handle(target, dsc->handle);
        }
        return 0;
    }

    mark_ancs_target_descriptor_done(target);
    if (error->status == BLE_HS_EDONE) {
        subscribe_discovered_ancs_cccd(conn_handle, target);
    } else {
        ESP_LOGW(TAG, "ANCS %s descriptor discovery failed: %d", ancs_target_label(target), error->status);
        discover_next_ancs_cccd(conn_handle);
    }
    return 0;
}

void discover_ancs_cccd(uint16_t conn_handle, AncsSubscribeTarget target)
{
    uint16_t value_handle = ancs_target_value_handle(target);
    uint16_t end_handle = ancs_descriptor_end_handle(target);
    if (value_handle == 0 || end_handle <= value_handle) {
        ESP_LOGW(TAG, "ANCS %s descriptor range unavailable", ancs_target_label(target));
        mark_ancs_target_descriptor_done(target);
        discover_next_ancs_cccd(conn_handle);
        return;
    }

    int rc = ble_gattc_disc_all_dscs(conn_handle, value_handle, end_handle, ancs_dsc_discovered_cb,
                                     ancs_target_arg(target));
    if (rc != 0) {
        ESP_LOGW(TAG, "ANCS %s descriptor discovery start failed: %d", ancs_target_label(target), rc);
        mark_ancs_target_descriptor_done(target);
        discover_next_ancs_cccd(conn_handle);
    }
}

void discover_next_ancs_cccd(uint16_t conn_handle)
{
    if (conn_handle != g_ancs.conn_handle) {
        return;
    }
    if (g_ancs.notification_source_handle != 0 &&
        !ancs_target_descriptor_done(ANCS_SUBSCRIBE_NOTIFICATION_SOURCE)) {
        discover_ancs_cccd(conn_handle, ANCS_SUBSCRIBE_NOTIFICATION_SOURCE);
        return;
    }
    if (g_ancs.data_source_handle != 0 && !ancs_target_descriptor_done(ANCS_SUBSCRIBE_DATA_SOURCE)) {
        discover_ancs_cccd(conn_handle, ANCS_SUBSCRIBE_DATA_SOURCE);
        return;
    }
    update_ancs_ready();
}

void request_ancs_notification_attributes(uint16_t conn_handle, const uint8_t* notification_uid)
{
    if (g_ancs.control_point_handle == 0 || notification_uid == nullptr) {
        return;
    }

    std::array<uint8_t, 32> command {};
    std::size_t index = 0;
    command[index++] = ANCS_COMMAND_GET_NOTIFICATION_ATTRIBUTES;
    std::memcpy(&command[index], notification_uid, 4);
    index += 4;

    auto add_attr = [&](uint8_t attr_id, uint16_t max_len) {
        command[index++] = attr_id;
        if (max_len > 0) {
            command[index++] = max_len & 0xFF;
            command[index++] = (max_len >> 8) & 0xFF;
        }
    };

    add_attr(ANCS_ATTR_APP_IDENTIFIER, 0);
    add_attr(ANCS_ATTR_TITLE, 96);
    add_attr(ANCS_ATTR_SUBTITLE, 96);
    add_attr(ANCS_ATTR_MESSAGE, 192);
    add_attr(ANCS_ATTR_DATE, 0);

    int rc = ble_gattc_write_flat(conn_handle, g_ancs.control_point_handle, command.data(), index, ancs_write_cb,
                                  const_cast<char*>("Control Point"));
    if (rc != 0) {
        ESP_LOGW(TAG, "failed to request ANCS attributes: %d", rc);
    }
}

void handle_ancs_notification_source(uint16_t conn_handle, os_mbuf* om)
{
    uint8_t payload[8] {};
    uint16_t copied = 0;
    int rc = ble_hs_mbuf_to_flat(om, payload, sizeof(payload), &copied);
    if (rc != 0 || copied < sizeof(payload)) {
        ESP_LOGW(TAG, "invalid ANCS notification source payload");
        return;
    }

    uint8_t event_id = payload[0];
    if (event_id == ANCS_EVENT_ADDED || event_id == ANCS_EVENT_MODIFIED) {
        request_ancs_notification_attributes(conn_handle, &payload[4]);
    }
}

int ancs_chr_discovered_cb(uint16_t conn_handle, const ble_gatt_error* error, const ble_gatt_chr* chr, void* arg)
{
    (void)arg;
    if (error->status == 0 && chr != nullptr) {
        if ((chr->properties & BLE_GATT_CHR_PROP_NOTIFY) && ble_uuid_cmp(&chr->uuid.u, &k_ancs_notification_source_uuid.u) == 0) {
            g_ancs.notification_source_def_handle = chr->def_handle;
            g_ancs.notification_source_handle = chr->val_handle;
            ESP_LOGI(TAG, "found ANCS Notification Source");
        } else if ((chr->properties & BLE_GATT_CHR_PROP_NOTIFY) && ble_uuid_cmp(&chr->uuid.u, &k_ancs_data_source_uuid.u) == 0) {
            g_ancs.data_source_def_handle = chr->def_handle;
            g_ancs.data_source_handle = chr->val_handle;
            ESP_LOGI(TAG, "found ANCS Data Source");
        } else if ((chr->properties & BLE_GATT_CHR_PROP_WRITE) && ble_uuid_cmp(&chr->uuid.u, &k_ancs_control_point_uuid.u) == 0) {
            g_ancs.control_point_def_handle = chr->def_handle;
            g_ancs.control_point_handle = chr->val_handle;
            ESP_LOGI(TAG, "found ANCS Control Point");
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        discover_next_ancs_cccd(conn_handle);
        if (g_ancs.control_point_handle == 0) {
            ESP_LOGW(TAG, "ANCS Control Point not found");
        }
        return 0;
    }

    ESP_LOGW(TAG, "ANCS characteristic discovery failed: %d", error->status);
    return 0;
}

int ancs_service_discovered_cb(uint16_t conn_handle, const ble_gatt_error* error, const ble_gatt_svc* svc, void* arg)
{
    (void)arg;
    if (error->status == 0 && svc != nullptr) {
        g_ancs.service_end_handle = svc->end_handle;
        int rc = ble_gattc_disc_all_chrs(conn_handle, svc->start_handle, svc->end_handle, ancs_chr_discovered_cb, nullptr);
        if (rc != 0) {
            ESP_LOGW(TAG, "ANCS characteristic discovery start failed: %d", rc);
        }
        return 0;
    }

    if (error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "ANCS service discovery failed: %d", error->status);
    }
    return 0;
}

void discover_ancs(uint16_t conn_handle)
{
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    reset_ancs_state(conn_handle);
    int rc = ble_gattc_disc_svc_by_uuid(conn_handle, &k_ancs_service_uuid.u, ancs_service_discovered_cb, nullptr);
    if (rc != 0) {
        ESP_LOGW(TAG, "ANCS service discovery start failed: %d", rc);
    }
}

#endif

void advertise()
{
    if (ble_gap_adv_active()) {
        return;
    }

    static constexpr uint8_t k_adv_data[] = {
        0x02, BLE_HS_ADV_TYPE_FLAGS, BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        0x11, BLE_HS_ADV_TYPE_SOL_UUIDS128,
        0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4, 0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79,
        0x05, BLE_HS_ADV_TYPE_INCOMP_NAME, 'M', '5', 'S', 'W',
        0x03, BLE_HS_ADV_TYPE_APPEARANCE, 0xC0, 0x00,
    };
    static_assert(sizeof(k_adv_data) <= BLE_HS_ADV_MAX_SZ, "advertising data must fit legacy BLE advertising");

    int rc = ble_gap_adv_set_data(k_adv_data, sizeof(k_adv_data));
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields failed: %d", rc);
        return;
    }

    static constexpr uint8_t k_scan_response_data[] = {
        0x11, BLE_HS_ADV_TYPE_COMP_UUIDS128,
        0x00, 0x57, 0x54, 0x53, 0x4D, 0x53, 0x65, 0x9D, 0x35, 0x4D, 0x6F, 0x6D, 0x01, 0x00, 0x3F, 0x7B,
        0x0C, BLE_HS_ADV_TYPE_COMP_NAME, 'M', '5', 'S', 't', 'o', 'p', 'W', 'a', 't', 'c', 'h',
    };
    static_assert(sizeof(k_scan_response_data) <= BLE_HS_ADV_MAX_SZ, "scan response data must fit legacy BLE advertising");

    rc = ble_gap_adv_rsp_set_data(k_scan_response_data, sizeof(k_scan_response_data));
    if (rc != 0) {
        ESP_LOGW(TAG, "scan response fields failed: %d", rc);
    }

    ble_gap_adv_params params {};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
    params.itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MAX;

    rc = ble_gap_adv_start(g_own_addr_type, nullptr, BLE_HS_FOREVER, &params, [](ble_gap_event* event, void* arg) -> int {
        (void)arg;
        switch (event->type) {
            case BLE_GAP_EVENT_CONNECT:
                if (event->connect.status == 0) {
                    ESP_LOGI(TAG, "phone notification client connected");
                    set_connected(true);
                    reset_ancs_state(event->connect.conn_handle);
                    int security_rc = ble_gap_security_initiate(event->connect.conn_handle);
                    if (security_rc != 0) {
                        ESP_LOGW(TAG, "security initiate failed: %d", security_rc);
                    }
                } else {
                    advertise();
                }
                return 0;
            case BLE_GAP_EVENT_DISCONNECT:
                ESP_LOGI(TAG, "phone notification client disconnected");
                set_connected(false);
                reset_ancs_state();
                advertise();
                return 0;
            case BLE_GAP_EVENT_ENC_CHANGE:
#if CONFIG_BT_NIMBLE_GATT_CLIENT
                if (event->enc_change.status == 0) {
                    discover_ancs(event->enc_change.conn_handle);
                } else {
                    ESP_LOGW(TAG, "encryption change failed: %d", event->enc_change.status);
                }
#endif
                return 0;
            case BLE_GAP_EVENT_NOTIFY_RX:
#if CONFIG_BT_NIMBLE_GATT_CLIENT
                if (event->notify_rx.attr_handle == g_ancs.notification_source_handle) {
                    handle_ancs_notification_source(event->notify_rx.conn_handle, event->notify_rx.om);
                } else if (event->notify_rx.attr_handle == g_ancs.data_source_handle) {
                    append_ancs_data_source(event->notify_rx.om);
                }
#endif
                return 0;
            case BLE_GAP_EVENT_MTU:
                g_mtu_size = event->mtu.value;
                return 0;
            case BLE_GAP_EVENT_REPEAT_PAIRING: {
                ble_gap_conn_desc desc {};
                int rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
                if (rc == 0) {
                    ble_store_util_delete_peer(&desc.peer_id_addr);
                }
                return BLE_GAP_REPEAT_PAIRING_RETRY;
            }
            case BLE_GAP_EVENT_ADV_COMPLETE:
                advertise();
                return 0;
            default:
                return 0;
        }
    }, nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "advertise failed: %d", rc);
    }
}

void on_ble_sync()
{
    int rc = ble_hs_id_infer_auto(0, &g_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "address infer failed: %d", rc);
        return;
    }

    g_ble_ready = true;
    advertise();
}

void on_ble_reset(int reason)
{
    g_ble_ready = false;
    g_connected = false;
    ESP_LOGW(TAG, "BLE reset: %d", reason);
}

void host_task(void* param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

}  // namespace

#endif

void phone_notifications_init()
{
    ensure_lock();
    if (g_init_started) {
        return;
    }
    g_init_started = true;

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
    int rc = nimble_port_init();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "nimble init failed: %d", rc);
        return;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("M5StopWatch");

    rc = ble_gatts_count_cfg(k_services);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatt count failed: %d", rc);
        return;
    }

    rc = ble_gatts_add_svcs(k_services);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatt add failed: %d", rc);
        return;
    }

    ble_hs_cfg.reset_cb = on_ble_reset;
    ble_hs_cfg.sync_cb = on_ble_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();

#if CONFIG_BT_NIMBLE_GATT_CLIENT
    ensure_ancs_buffer_lock();
    if (g_ancs_flush_timer == nullptr) {
        esp_timer_create_args_t timer_args {};
        timer_args.callback = ancs_flush_timer_callback;
        timer_args.name = "ancs_flush";
        esp_err_t timer_ret = esp_timer_create(&timer_args, &g_ancs_flush_timer);
        if (timer_ret != ESP_OK) {
            ESP_LOGW(TAG, "ANCS flush timer create failed: %s", esp_err_to_name(timer_ret));
        }
    }
#endif

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE notification receiver started");
#else
    ESP_LOGW(TAG, "BLE notification receiver disabled by sdkconfig");
#endif
}

bool phone_notifications_is_ble_ready()
{
    return g_ble_ready;
}

bool phone_notifications_is_connected()
{
    ScopedNotificationLock lock;
    return lock.locked() ? g_connected : false;
}

bool phone_notifications_is_ancs_ready()
{
    ScopedNotificationLock lock;
    return lock.locked() ? g_ancs_ready : false;
}

uint32_t phone_notifications_count()
{
    ScopedNotificationLock lock;
    return lock.locked() ? g_notifications.size() : 0;
}

uint32_t phone_notifications_latest_sequence()
{
    ScopedNotificationLock lock;
    return lock.locked() ? g_sequence : 0;
}

std::vector<PhoneNotification> phone_notifications_recent(std::size_t max_count)
{
    ScopedNotificationLock lock;
    if (!lock.locked()) {
        return {};
    }

    max_count = std::min(max_count, g_notifications.size());
    return std::vector<PhoneNotification>(g_notifications.begin(), g_notifications.begin() + max_count);
}

void phone_notifications_clear()
{
    ScopedNotificationLock lock;
    if (!lock.locked()) {
        return;
    }

    g_notifications.clear();
    ++g_sequence;
}

void phone_notifications_receive_payload(const std::string& payload)
{
    add_notification(parse_payload(payload));
}
