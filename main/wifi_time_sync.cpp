/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "wifi_time_sync.h"

#include <cstring>
#include <ctime>
#include <string_view>

#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_sntp.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <hal/hal.h>

#if __has_include("private_wifi_config.h")
#include "private_wifi_config.h"
#endif

#ifndef M5_WIFI_SSID
#define M5_WIFI_SSID ""
#endif

#ifndef M5_WIFI_PASSWORD
#define M5_WIFI_PASSWORD ""
#endif

namespace {

constexpr const char* _tag = "WiFi-Time";

constexpr EventBits_t _wifi_connected_bit = BIT0;
constexpr EventBits_t _wifi_failed_bit    = BIT1;
constexpr int _max_wifi_retry             = 10;
constexpr int _wifi_connect_timeout_ms    = 20000;
constexpr int _ntp_retry_count            = 15;
constexpr int _ntp_retry_delay_ms         = 1000;

EventGroupHandle_t _wifi_event_group = nullptr;
esp_netif_t* _sta_netif              = nullptr;
int _wifi_retry                      = 0;

bool has_wifi_credentials()
{
    return std::string_view(M5_WIFI_SSID).size() > 0;
}

void wifi_event_handler(void*, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (_wifi_retry < _max_wifi_retry) {
            _wifi_retry++;
            esp_wifi_connect();
            ESP_LOGW(_tag, "wifi disconnected, retry %d/%d", _wifi_retry, _max_wifi_retry);
        } else if (_wifi_event_group != nullptr) {
            xEventGroupSetBits(_wifi_event_group, _wifi_failed_bit);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(_tag, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        _wifi_retry = 0;
        if (_wifi_event_group != nullptr) {
            xEventGroupSetBits(_wifi_event_group, _wifi_connected_bit);
        }
    }
}

bool ensure_wifi_stack_ready()
{
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(_tag, "netif init failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(_tag, "event loop init failed: %s", esp_err_to_name(ret));
        return false;
    }

    if (_sta_netif == nullptr) {
        _sta_netif = esp_netif_create_default_wifi_sta();
        if (_sta_netif == nullptr) {
            ESP_LOGE(_tag, "failed to create wifi sta netif");
            return false;
        }
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable         = false;
    ret                    = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(_tag, "wifi init failed: %s", esp_err_to_name(ret));
        return false;
    }

    return true;
}

bool connect_wifi()
{
    _wifi_event_group = xEventGroupCreate();
    if (_wifi_event_group == nullptr) {
        ESP_LOGE(_tag, "failed to create event group");
        return false;
    }

    esp_event_handler_instance_t wifi_handler = nullptr;
    esp_event_handler_instance_t ip_handler   = nullptr;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr,
                                                       &wifi_handler));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr,
                                                       &ip_handler));

    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), M5_WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password), M5_WIFI_PASSWORD,
                 sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = std::string_view(M5_WIFI_PASSWORD).empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    const EventBits_t bits = xEventGroupWaitBits(_wifi_event_group, _wifi_connected_bit | _wifi_failed_bit, pdFALSE,
                                                 pdFALSE, pdMS_TO_TICKS(_wifi_connect_timeout_ms));

    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler);
    vEventGroupDelete(_wifi_event_group);
    _wifi_event_group = nullptr;

    if (bits & _wifi_connected_bit) {
        ESP_LOGI(_tag, "wifi connected");
        return true;
    }

    ESP_LOGE(_tag, "wifi connect failed or timed out");
    return false;
}

bool sync_sntp_time()
{
    esp_sntp_stop();
    esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "jp.pool.ntp.org");
    esp_sntp_setservername(1, "ntp.nict.jp");
    esp_sntp_init();

    bool synced = false;
    for (int retry = 0; retry < _ntp_retry_count; ++retry) {
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            std::time_t now = std::time(nullptr);
            struct tm time_info;
            localtime_r(&now, &time_info);
            ESP_LOGI(_tag, "time synced: %04d-%02d-%02d %02d:%02d:%02d", time_info.tm_year + 1900,
                     time_info.tm_mon + 1, time_info.tm_mday, time_info.tm_hour, time_info.tm_min, time_info.tm_sec);
            synced = true;
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(_ntp_retry_delay_ms));
    }

    esp_sntp_stop();
    return synced;
}

void stop_wifi()
{
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_NULL);
}

}  // namespace

bool sync_time_from_wifi()
{
    if (!has_wifi_credentials()) {
        ESP_LOGW(_tag, "wifi credentials are not configured; skipping time sync");
        return false;
    }

    ESP_LOGI(_tag, "sync time using wifi ssid: %s", M5_WIFI_SSID);

    if (!ensure_wifi_stack_ready()) {
        return false;
    }

    const bool connected = connect_wifi();
    if (!connected) {
        stop_wifi();
        return false;
    }

    bool synced = false;
    if (sync_sntp_time()) {
        GetHAL().syncSystemTimeToRtc();
        synced = true;
    } else {
        ESP_LOGE(_tag, "ntp sync failed");
    }

    stop_wifi();
    return synced;
}
