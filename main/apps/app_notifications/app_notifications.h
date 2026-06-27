/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <apps/common/key_manager/key_manager.h>
#include <array>
#include <lvgl.h>
#include <memory>
#include <mooncake.h>

class AppNotifications : public mooncake::AppAbility {
public:
    AppNotifications();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    std::unique_ptr<input::KeyManager> _key_manager;
    lv_obj_t* _root        = nullptr;
    lv_obj_t* _status      = nullptr;
    lv_obj_t* _count       = nullptr;
    lv_obj_t* _time        = nullptr;
    lv_obj_t* _app         = nullptr;
    lv_obj_t* _title       = nullptr;
    lv_obj_t* _body        = nullptr;
    std::array<lv_obj_t*, 3> _history {};
    uint32_t _last_sequence = UINT32_MAX;
    uint32_t _last_refresh  = 0;

    void create_ui();
    void update_ui(bool force = false);
};
