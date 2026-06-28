/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <mooncake_log.h>
#include <mooncake.h>
#include <apps/apps.h>
#include <hal/hal.h>
#include <lv_demos.h>
#include <apps/common/audio/audio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "wifi_time_sync.h"
#include "phone_notifications.h"

using namespace mooncake;
using namespace smooth_ui_toolkit;

namespace {

constexpr const char* kBootServicesTag = "BootServices";

void boot_services_task(void*)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    phone_notifications_init();
    sync_time_from_wifi();
    vTaskDelete(nullptr);
}

void start_boot_services()
{
    const BaseType_t result = xTaskCreate(boot_services_task, "boot_services", 8192, nullptr, 1, nullptr);
    if (result != pdPASS) {
        mclog::tagWarn(kBootServicesTag, "failed to start boot services task");
    }
}

}  // namespace

extern "C" void app_main(void)
{
    // Setup logger
    mclog::set_level(mclog::level_info);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);

    // HAL init
    GetHAL().init();
    GetHAL().setTimezone("JST-9");

    // Setup ui hal
    ui_hal::on_delay([](uint32_t ms) { GetHAL().delay(ms); });
    ui_hal::on_get_tick([]() { return GetHAL().millis(); });

    // Install apps
    GetMooncake().installApp(std::make_unique<AppLauncher>());
    GetMooncake().installApp(std::make_unique<AppAlarmClock>());
    GetMooncake().installApp(std::make_unique<AppWatchFace>());
    GetMooncake().installApp(std::make_unique<AppStopWatch>());
    GetMooncake().installApp(std::make_unique<AppBadge>());
    GetMooncake().installApp(std::make_unique<AppNotifications>());
    GetMooncake().installApp(std::make_unique<AppImu>());
    GetMooncake().installApp(std::make_unique<AppFft>());
    GetMooncake().installApp(std::make_unique<AppLuckyWheel>());
    GetMooncake().installApp(std::make_unique<AppGuruguru>());
    GetMooncake().installApp(std::make_unique<AppStackRemote>());
    GetMooncake().installApp(std::make_unique<AppSetup>());
    // GetMooncake().installApp(std::make_unique<AppTemplate>());

    start_boot_services();

    // Main loop
    while (1) {
        GetHAL().feedTheDog();
        GetMooncake().update();
    }
}
