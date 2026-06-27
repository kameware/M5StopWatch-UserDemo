/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "phone_notifications.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
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
#include <host/ble_uuid.h>
#include <host/ble_store.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <os/os_mbuf.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>
#include <store/config/ble_store_config.h>

extern "C" void ble_store_config_init(void);

namespace {

uint8_t g_own_addr_type = 0;

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

void set_connected(bool connected)
{
    ScopedNotificationLock lock;
    if (lock.locked()) {
        g_connected = connected;
    }
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

void advertise()
{
    if (ble_gap_adv_active()) {
        return;
    }

    ble_hs_adv_fields fields {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &k_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields failed: %d", rc);
        return;
    }

    const char* name = ble_svc_gap_device_name();
    ble_hs_adv_fields response {};
    response.name = reinterpret_cast<const uint8_t*>(name);
    response.name_len = std::strlen(name);
    response.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&response);
    if (rc != 0) {
        ESP_LOGW(TAG, "scan response fields failed: %d", rc);
    }

    ble_gap_adv_params params {};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(g_own_addr_type, nullptr, BLE_HS_FOREVER, &params, [](ble_gap_event* event, void* arg) -> int {
        (void)arg;
        switch (event->type) {
            case BLE_GAP_EVENT_CONNECT:
                if (event->connect.status == 0) {
                    ESP_LOGI(TAG, "phone notification client connected");
                    set_connected(true);
                } else {
                    advertise();
                }
                return 0;
            case BLE_GAP_EVENT_DISCONNECT:
                ESP_LOGI(TAG, "phone notification client disconnected");
                set_connected(false);
                advertise();
                return 0;
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
    ble_store_config_init();

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
