/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_notifications.h"
#include <assets/assets.h>
#include <hal/hal.h>
#include <mooncake_log.h>
#include <phone_notifications.h>

using namespace mooncake;

namespace {

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, uint32_t color, int width, lv_text_align_t align)
{
    auto* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(label, align, 0);
    return label;
}

void clear_button_event(lv_event_t* event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        phone_notifications_clear();
    }
}

}  // namespace

AppNotifications::AppNotifications()
{
    setAppInfo().name = "Notify";
    setAppInfo().icon = (void*)&icon_badge;
}

void AppNotifications::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppNotifications::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    phone_notifications_init();
    _key_manager = std::make_unique<input::KeyManager>();

    LvglLockGuard lock;
    create_ui();
    update_ui(true);
}

void AppNotifications::onRunning()
{
    if (_key_manager) {
        switch (_key_manager->update()) {
            case input::KeyEvent::GoHome:
                close();
                return;
            case input::KeyEvent::GoPrevious:
                phone_notifications_clear();
                break;
            default:
                break;
        }
    }

    uint32_t now = GetHAL().millis();
    if (now - _last_refresh > 500 || phone_notifications_latest_sequence() != _last_sequence) {
        LvglLockGuard lock;
        update_ui();
        _last_refresh = now;
    }
}

void AppNotifications::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    _key_manager.reset();

    LvglLockGuard lock;
    if (_root != nullptr) {
        lv_obj_delete(_root);
        _root = nullptr;
    }
}

void AppNotifications::create_ui()
{
    _root = lv_obj_create(lv_screen_active());
    lv_obj_set_size(_root, 466, 466);
    lv_obj_align(_root, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_root, lv_color_black(), 0);
    lv_obj_set_style_border_width(_root, 0, 0);
    lv_obj_set_style_radius(_root, 0, 0);
    lv_obj_set_style_pad_all(_root, 0, 0);
    lv_obj_remove_flag(_root, LV_OBJ_FLAG_SCROLLABLE);

    auto* heading = make_label(_root, &MontserratSemiBold26, 0xFFFFFF, 320, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(heading, "Notifications");
    lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 30);

    _status = make_label(_root, &lv_font_montserrat_18, 0x92E8FF, 360, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(_status, LV_ALIGN_TOP_MID, 0, 72);

    _count = make_label(_root, &lv_font_montserrat_18, 0xCFCFCF, 360, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(_count, LV_ALIGN_TOP_MID, 0, 100);

    auto* panel = lv_obj_create(_root);
    lv_obj_set_size(panel, 394, 188);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 140);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x16191D), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x3A414A), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_pad_all(panel, 16, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    _time = make_label(panel, &lv_font_montserrat_18, 0x8EA0B5, 150, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(_time, LV_ALIGN_TOP_LEFT, 0, 0);

    _app = make_label(panel, &lv_font_montserrat_18, 0x8EA0B5, 170, LV_TEXT_ALIGN_RIGHT);
    lv_obj_align(_app, LV_ALIGN_TOP_RIGHT, 0, 0);

    _title = make_label(panel, &MontserratSemiBold26, 0xFFFFFF, 350, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(_title, LV_ALIGN_TOP_LEFT, 0, 36);

    _body = make_label(panel, &lv_font_montserrat_18, 0xDADDE2, 350, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(_body, LV_ALIGN_TOP_LEFT, 0, 82);

    for (int i = 0; i < _history.size(); ++i) {
        _history[i] = make_label(_root, &lv_font_montserrat_18, 0xAEB7C2, 390, LV_TEXT_ALIGN_LEFT);
        lv_obj_align(_history[i], LV_ALIGN_TOP_MID, 0, 344 + i * 28);
        lv_label_set_long_mode(_history[i], LV_LABEL_LONG_MODE_DOTS);
    }

    auto* clear_button = lv_button_create(_root);
    lv_obj_set_size(clear_button, 128, 44);
    lv_obj_align(clear_button, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_set_style_bg_color(clear_button, lv_color_hex(0x2D3742), 0);
    lv_obj_set_style_border_width(clear_button, 0, 0);
    lv_obj_set_style_radius(clear_button, 8, 0);
    lv_obj_add_event_cb(clear_button, clear_button_event, LV_EVENT_CLICKED, nullptr);

    auto* clear_label = make_label(clear_button, &lv_font_montserrat_18, 0xFFFFFF, 96, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(clear_label, "Clear");
    lv_obj_center(clear_label);
}

void AppNotifications::update_ui(bool force)
{
    uint32_t sequence = phone_notifications_latest_sequence();
    if (!force && sequence == _last_sequence) {
        return;
    }
    _last_sequence = sequence;

    const bool ready     = phone_notifications_is_ble_ready();
    const bool connected = phone_notifications_is_connected();
    auto recent          = phone_notifications_recent(4);

    lv_label_set_text(_status, connected ? "BLE connected" : (ready ? "BLE waiting" : "BLE starting"));

    char count_text[32] {};
    std::snprintf(count_text, sizeof(count_text), "%lu received", static_cast<unsigned long>(phone_notifications_count()));
    lv_label_set_text(_count, count_text);

    if (recent.empty()) {
        lv_label_set_text(_time, "--/-- --:--");
        lv_label_set_text(_app, "Phone");
        lv_label_set_text(_title, "No notification");
        lv_label_set_text(_body, "Waiting for incoming phone notifications.");
    } else {
        const auto& latest = recent.front();
        lv_label_set_text(_time, latest.receivedAt.c_str());
        lv_label_set_text(_app, latest.app.c_str());
        lv_label_set_text(_title, latest.title.c_str());
        lv_label_set_text(_body, latest.body.c_str());
    }

    for (int i = 0; i < _history.size(); ++i) {
        if (i + 1 < recent.size()) {
            auto line = recent[i + 1].receivedAt + "  " + recent[i + 1].app + "  " + recent[i + 1].title;
            lv_label_set_text(_history[i], line.c_str());
        } else {
            lv_label_set_text(_history[i], "");
        }
    }
}
