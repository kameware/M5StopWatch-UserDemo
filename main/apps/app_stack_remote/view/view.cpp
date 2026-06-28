/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "view.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <espnow.h>
#include <freertos/FreeRTOS.h>
#include <hal/hal.h>

using namespace view;

namespace {

constexpr const char* _tag = "StackRemote";

constexpr int16_t _yaw_min       = -1280;
constexpr int16_t _yaw_max       = 1280;
constexpr int16_t _pitch_min     = 0;
constexpr int16_t _pitch_max     = 900;
constexpr int16_t _pitch_home    = 450;
constexpr uint16_t _send_ms      = 40;
constexpr uint16_t _long_press_ms = 650;
constexpr uint16_t _gyro_read_ms  = 20;
constexpr float _gyro_roll_range  = 0.6f;
constexpr float _gyro_pitch_range = 0.7f;
constexpr float _imu_min_g        = 0.05f;

constexpr uint16_t _color_bg           = 0x0841;
constexpr uint16_t _color_panel        = 0x18e3;
constexpr uint16_t _color_panel_active = 0x2d6b;
constexpr uint16_t _color_pad          = 0x1082;
constexpr uint16_t _color_pad_line     = 0x39e7;
constexpr uint16_t _color_accent       = 0x07ff;
constexpr uint16_t _color_warn         = 0xfd20;
constexpr uint16_t _color_text         = 0xffff;
constexpr uint16_t _color_muted        = 0xbdf7;
constexpr uint16_t _color_error        = 0xf986;

bool okOrInvalidState(esp_err_t ret)
{
    return ret == ESP_OK || ret == ESP_ERR_INVALID_STATE;
}

int16_t clampInt(int32_t value, int16_t minValue, int16_t maxValue)
{
    if (value < minValue) {
        return minValue;
    }
    if (value > maxValue) {
        return maxValue;
    }
    return static_cast<int16_t>(value);
}

int16_t atLeastOne(int16_t value)
{
    return value > 1 ? value : 1;
}

float clampFloat(float value, float minValue, float maxValue)
{
    if (value < minValue) {
        return minValue;
    }
    if (value > maxValue) {
        return maxValue;
    }
    return value;
}

int16_t smoothControl(int16_t current, int16_t target)
{
    const int16_t diff = target - current;
    if (std::abs(diff) < 4) {
        return target;
    }
    return current + diff / 3;
}

float angleDelta(float value, float origin)
{
    float delta = value - origin;
    while (delta > static_cast<float>(M_PI)) {
        delta -= static_cast<float>(M_PI * 2.0);
    }
    while (delta < static_cast<float>(-M_PI)) {
        delta += static_cast<float>(M_PI * 2.0);
    }
    return delta;
}

void writeLe16(uint8_t* buffer, size_t offset, int16_t value)
{
    buffer[offset]     = static_cast<uint8_t>(value & 0xff);
    buffer[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

bool ensureWifiReady()
{
    esp_err_t ret = esp_netif_init();
    if (!okOrInvalidState(ret)) {
        ESP_LOGE(_tag, "netif init failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_event_loop_create_default();
    if (!okOrInvalidState(ret)) {
        ESP_LOGE(_tag, "event loop init failed: %s", esp_err_to_name(ret));
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable         = false;
    ret                    = esp_wifi_init(&cfg);
    if (!okOrInvalidState(ret)) {
        ESP_LOGE(_tag, "wifi init failed: %s", esp_err_to_name(ret));
        return false;
    }

    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGW(_tag, "wifi mode sta failed: %s", esp_err_to_name(ret));
    }

    esp_wifi_set_ps(WIFI_PS_NONE);
    ret = esp_wifi_start();
    if (!okOrInvalidState(ret)) {
        ESP_LOGE(_tag, "wifi start failed: %s", esp_err_to_name(ret));
        return false;
    }

    return true;
}

}  // namespace

StackRemoteView::~StackRemoteView()
{
    deinitEspNow();
}

void StackRemoteView::init()
{
    auto& display = GetHAL().getDisplay();
    _screen_w     = display.width();
    _screen_h     = display.height();

    _canvas = std::make_unique<M5Canvas>(&display);
    _canvas->setColorDepth(16);
    if (_canvas->createSprite(_screen_w, _screen_h) == nullptr) {
        _canvas.reset();
        display.fillScreen(TFT_BLACK);
        return;
    }

    buildLayout();
    initEspNow();
    homePose(true);
    drawIfNeeded(true);
}

void StackRemoteView::update()
{
    handleTouch();
    updateGyroControl();
    sendPose();
    drawIfNeeded();
}

void StackRemoteView::buildLayout()
{
    const int16_t controlMargin = 48;
    const int16_t padMargin     = 48;
    const int16_t gap           = 4;
    const int16_t topY          = 74;
    const int16_t topH          = 38;
    const int16_t bottomH       = 40;
    const int16_t bottomY       = _screen_h - bottomH - 72;
    const int16_t buttonW       = (_screen_w - controlMargin * 2 - gap * 4) / 5;

    _channel_button = {controlMargin, topY, buttonW, topH};
    _target_button  = {static_cast<int16_t>(_channel_button.x + buttonW + gap), topY, buttonW, topH};
    _speed_button   = {static_cast<int16_t>(_target_button.x + buttonW + gap), topY, buttonW, topH};
    _mode_button    = {static_cast<int16_t>(_speed_button.x + buttonW + gap), topY, buttonW, topH};
    _laser_button   = {static_cast<int16_t>(_mode_button.x + buttonW + gap), topY,
                       static_cast<int16_t>(_screen_w - controlMargin - _mode_button.x - buttonW - gap), topH};

    const int16_t padY = topY + topH + 14;
    _pad_rect          = {padMargin, padY, static_cast<int16_t>(_screen_w - padMargin * 2),
                          static_cast<int16_t>(bottomY - padY - 14)};

    const int16_t homeW = (_screen_w - controlMargin * 2 - gap) / 2;
    _home_button        = {controlMargin, bottomY, homeW, bottomH};
    _hold_button        = {static_cast<int16_t>(_home_button.x + homeW + gap), bottomY,
                           static_cast<int16_t>(_screen_w - controlMargin - _home_button.x - homeW - gap), bottomH};
}

void StackRemoteView::initEspNow()
{
    _remote.espNowReady = false;

    if (!ensureWifiReady()) {
        return;
    }

    setWifiChannel(_remote.channel);
    espnow_deinit();

    espnow_config_t config = ESPNOW_INIT_CONFIG_DEFAULT();
    config.forward_enable         = false;
    config.forward_switch_channel = false;
    config.send_retry_num         = 5;
    config.receive_enable.forward = false;
    config.receive_enable.data    = false;

    esp_err_t ret = espnow_init(&config);
    _remote.lastError = ret;
    if (ret != ESP_OK) {
        ESP_LOGE(_tag, "espnow init failed: %s", esp_err_to_name(ret));
        return;
    }

    _remote.espNowReady = true;
}

void StackRemoteView::deinitEspNow()
{
    espnow_deinit();
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_NULL);
}

void StackRemoteView::setWifiChannel(uint8_t channel)
{
    esp_wifi_set_promiscuous(true);
    esp_err_t ret = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);

    _remote.lastError = ret;
    if (ret != ESP_OK) {
        ESP_LOGE(_tag, "set channel %u failed: %s", channel, esp_err_to_name(ret));
    }
}

void StackRemoteView::sendPose(bool force)
{
    const uint32_t now = GetHAL().millis();
    if (!force && now - _last_send_at < _send_ms) {
        return;
    }

    const bool changed = !_has_last_packet || _remote.targetId != _last_target_id ||
                         std::abs(_remote.yaw - _last_yaw) >= 5 ||
                         std::abs(_remote.pitch - _last_pitch) >= 5 ||
                         _remote.speed != _last_speed || _remote.laser != _last_laser;
    if (!force && !changed) {
        return;
    }

    if (!_remote.espNowReady) {
        _remote.lastSendOk = false;
        return;
    }

    uint8_t packet[8] = {};
    packet[0] = _remote.targetId;
    writeLe16(packet, 1, _remote.yaw);
    writeLe16(packet, 3, _remote.pitch);
    writeLe16(packet, 5, _remote.speed);
    packet[7] = _remote.laser ? 1 : 0;

    espnow_frame_head_t frameHead = ESPNOW_FRAME_CONFIG_DEFAULT();
    esp_err_t ret = espnow_send(ESPNOW_DATA_TYPE_DATA, ESPNOW_ADDR_BROADCAST, packet, sizeof(packet), &frameHead,
                                portMAX_DELAY);
    _remote.lastError  = ret;
    _remote.lastSendOk = (ret == ESP_OK);

    if (_remote.lastSendOk) {
        _remote.sentCount++;
        _has_last_packet = true;
        _last_target_id  = _remote.targetId;
        _last_yaw        = _remote.yaw;
        _last_pitch      = _remote.pitch;
        _last_speed      = _remote.speed;
        _last_laser      = _remote.laser;
        _last_send_at    = now;
    }
}

void StackRemoteView::homePose(bool force)
{
    _remote.yaw   = 0;
    _remote.pitch = _pitch_home;
    sendPose(force);
}

void StackRemoteView::handleTouch()
{
    auto point = GetHAL().getTouchPoint();
    const bool isDown = point.num > 0;
    const uint32_t now = GetHAL().millis();

    if (isDown && !_touch.wasDown) {
        _touch.activeArea  = hitTest(point.x, point.y);
        _touch.downAt      = now;
        _touch.longApplied = false;
    }

    if (isDown) {
        if (_touch.activeArea == HitArea::Pad && !_remote.gyroMode) {
            _remote.yaw   = touchToYaw(point.x);
            _remote.pitch = touchToPitch(point.y);
            sendPose();
        } else if (!_touch.longApplied && now - _touch.downAt > _long_press_ms) {
            handleControl(_touch.activeArea, -1);
            _touch.longApplied = true;
        }
    }

    if (!isDown && _touch.wasDown) {
        if (_touch.activeArea == HitArea::Pad) {
            if (!_remote.gyroMode && !_remote.holdOnRelease) {
                homePose(true);
            }
        } else if (!_touch.longApplied) {
            handleControl(_touch.activeArea, 1);
        }
        _touch.activeArea = HitArea::None;
    }

    _touch.wasDown = isDown;
}

void StackRemoteView::handleControl(HitArea area, int direction)
{
    switch (area) {
        case HitArea::Channel: {
            int next = static_cast<int>(_remote.channel) + direction;
            if (next > 13) {
                next = 1;
            } else if (next < 1) {
                next = 13;
            }
            _remote.channel = static_cast<uint8_t>(next);
            initEspNow();
            sendPose(true);
            break;
        }
        case HitArea::Target: {
            int next = static_cast<int>(_remote.targetId) + direction;
            if (next > 254) {
                next = 0;
            } else if (next < 0) {
                next = 254;
            }
            _remote.targetId = static_cast<uint8_t>(next);
            sendPose(true);
            break;
        }
        case HitArea::Speed: {
            constexpr int16_t speeds[] = {200, 400, 600, 800, 1000};
            int index = 2;
            for (int i = 0; i < 5; ++i) {
                if (_remote.speed == speeds[i]) {
                    index = i;
                    break;
                }
            }
            index += direction;
            if (index < 0) {
                index = 4;
            } else if (index > 4) {
                index = 0;
            }
            _remote.speed = speeds[index];
            sendPose(true);
            break;
        }
        case HitArea::Mode:
            _remote.gyroMode = !_remote.gyroMode;
            if (_remote.gyroMode) {
                calibrateGyro();
                homePose(true);
            } else if (!_remote.holdOnRelease) {
                homePose(true);
            }
            break;
        case HitArea::Laser:
            _remote.laser = !_remote.laser;
            sendPose(true);
            break;
        case HitArea::Home:
            homePose(true);
            break;
        case HitArea::Hold:
            _remote.holdOnRelease = !_remote.holdOnRelease;
            if (!_remote.holdOnRelease) {
                homePose(true);
            }
            break;
        default:
            break;
    }
}

void StackRemoteView::updateGyroControl()
{
    if (!_remote.gyroMode) {
        return;
    }

    const uint32_t now = GetHAL().millis();
    if (now - _last_gyro_at < _gyro_read_ms) {
        return;
    }
    _last_gyro_at = now;

    float roll  = 0.0f;
    float pitch = 0.0f;
    if (!readImuAngles(roll, pitch)) {
        return;
    }

    _gyro_roll  = roll;
    _gyro_pitch = pitch;
    if (!_gyro_calibrated) {
        _gyro_neutral_roll  = roll;
        _gyro_neutral_pitch = pitch;
        _gyro_calibrated    = true;
    }

    const float rollDelta  = clampFloat(roll - _gyro_neutral_roll, -_gyro_roll_range, _gyro_roll_range);
    const float pitchDelta = clampFloat(angleDelta(pitch, _gyro_neutral_pitch), -_gyro_pitch_range, _gyro_pitch_range);

    const int16_t targetYaw = clampInt(
        static_cast<int32_t>(std::round((rollDelta / _gyro_roll_range) * _yaw_max)), _yaw_min, _yaw_max);
    const int16_t targetPitch = clampInt(
        _pitch_home - static_cast<int32_t>(std::round((pitchDelta / _gyro_pitch_range) * _pitch_home)), _pitch_min,
        _pitch_max);

    _remote.yaw   = smoothControl(_remote.yaw, targetYaw);
    _remote.pitch = smoothControl(_remote.pitch, targetPitch);
}

bool StackRemoteView::readImuAngles(float& roll, float& pitch)
{
    GetHAL().updateImuData();
    const auto& imu = GetHAL().getImuData();

    const float ax = imu.accelX;
    const float ay = imu.accelY;
    const float az = imu.accelZ;
    if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(az)) {
        return false;
    }

    const float magnitude = std::sqrt(ax * ax + ay * ay + az * az);
    if (magnitude < _imu_min_g) {
        return false;
    }

    pitch = std::atan2(az, ay);
    roll  = std::atan2(ax, std::sqrt(ay * ay + az * az));
    return true;
}

void StackRemoteView::calibrateGyro()
{
    float roll  = 0.0f;
    float pitch = 0.0f;
    if (readImuAngles(roll, pitch)) {
        _gyro_neutral_roll  = roll;
        _gyro_neutral_pitch = pitch;
        _gyro_roll          = roll;
        _gyro_pitch         = pitch;
        _gyro_calibrated    = true;
    } else {
        _gyro_calibrated = false;
    }
}

void StackRemoteView::drawIfNeeded(bool force)
{
    const uint32_t now = GetHAL().millis();
    if (!force && now - _last_draw_at < 50) {
        return;
    }

    _last_draw_at = now;
    drawUi();
}

void StackRemoteView::drawUi()
{
    if (!_canvas) {
        return;
    }

    _canvas->fillScreen(_color_bg);
    _canvas->setTextSize(1);

    char label[24];
    snprintf(label, sizeof(label), "CH %u", _remote.channel);
    drawButton(_channel_button, label, _touch.activeArea == HitArea::Channel);

    snprintf(label, sizeof(label), "ID %u", _remote.targetId);
    drawButton(_target_button, label, _touch.activeArea == HitArea::Target);

    snprintf(label, sizeof(label), "S %d", _remote.speed);
    drawButton(_speed_button, label, _touch.activeArea == HitArea::Speed);

    drawButton(_mode_button, _remote.gyroMode ? "GYRO" : "TOUCH",
               _remote.gyroMode || _touch.activeArea == HitArea::Mode);

    drawButton(_laser_button, _remote.laser ? "LAS ON" : "LAS",
               _remote.laser || _touch.activeArea == HitArea::Laser);

    _canvas->fillRoundRect(_pad_rect.x, _pad_rect.y, _pad_rect.w, _pad_rect.h, 16,
                           _remote.gyroMode ? _color_panel_active : _color_pad);
    _canvas->drawRoundRect(_pad_rect.x, _pad_rect.y, _pad_rect.w, _pad_rect.h, 16, _color_pad_line);

    const int16_t centerX = yawToX(0);
    const int16_t centerY = pitchToY(_pitch_home);
    _canvas->drawFastVLine(centerX, _pad_rect.y + 14, _pad_rect.h - 28, _color_pad_line);
    _canvas->drawFastHLine(_pad_rect.x + 14, centerY, _pad_rect.w - 28, _color_pad_line);

    const int16_t knobX = yawToX(_remote.yaw);
    const int16_t knobY = pitchToY(_remote.pitch);
    _canvas->fillCircle(knobX, knobY, 16, _remote.laser ? _color_warn : _color_accent);
    _canvas->drawCircle(knobX, knobY, 20, _color_text);

    const int16_t statusY = _pad_rect.y + _pad_rect.h - 24;
    snprintf(label, sizeof(label), "Y %+4d  P %03d", _remote.yaw, _remote.pitch);
    _canvas->setTextColor(_color_muted);
    _canvas->setCursor(_pad_rect.x + 14, statusY);
    _canvas->print(label);

    _canvas->setTextColor(_remote.espNowReady && _remote.lastSendOk ? _color_accent : _color_error);
    snprintf(label, sizeof(label), "%s %lu", _remote.espNowReady ? (_remote.lastSendOk ? "SEND" : "READY") : "ERR",
             static_cast<unsigned long>(_remote.sentCount));
    _canvas->setCursor(_pad_rect.x + _pad_rect.w - _canvas->textWidth(label) - 14, statusY);
    _canvas->print(label);

    drawButton(_home_button, "HOME", _touch.activeArea == HitArea::Home);
    drawButton(_hold_button, _remote.holdOnRelease ? "HOLD" : "SPRING",
               _remote.holdOnRelease || _touch.activeArea == HitArea::Hold);

    auto& display = GetHAL().getDisplay();
    display.startWrite();
    _canvas->pushSprite(&display, 0, 0);
    display.endWrite();
}

void StackRemoteView::drawButton(const Rect& rect, const char* text, bool active)
{
    const uint16_t fill = active ? _color_panel_active : _color_panel;
    _canvas->fillRoundRect(rect.x, rect.y, rect.w, rect.h, 10, fill);
    _canvas->drawRoundRect(rect.x, rect.y, rect.w, rect.h, 10, active ? _color_accent : _color_pad_line);
    drawCenteredText(rect, text, active ? _color_accent : _color_text);
}

void StackRemoteView::drawCenteredText(const Rect& rect, const char* text, uint16_t color)
{
    _canvas->setTextColor(color);
    const int16_t textW = _canvas->textWidth(text);
    const int16_t textH = _canvas->fontHeight();
    _canvas->setCursor(rect.x + (rect.w - textW) / 2, rect.y + (rect.h - textH) / 2);
    _canvas->print(text);
}

StackRemoteView::HitArea StackRemoteView::hitTest(int16_t x, int16_t y) const
{
    if (_channel_button.contains(x, y)) {
        return HitArea::Channel;
    }
    if (_target_button.contains(x, y)) {
        return HitArea::Target;
    }
    if (_speed_button.contains(x, y)) {
        return HitArea::Speed;
    }
    if (_mode_button.contains(x, y)) {
        return HitArea::Mode;
    }
    if (_laser_button.contains(x, y)) {
        return HitArea::Laser;
    }
    if (_home_button.contains(x, y)) {
        return HitArea::Home;
    }
    if (_hold_button.contains(x, y)) {
        return HitArea::Hold;
    }
    if (_pad_rect.contains(x, y)) {
        return HitArea::Pad;
    }
    return HitArea::None;
}

int16_t StackRemoteView::touchToYaw(int16_t x) const
{
    const int16_t usableW = atLeastOne(_pad_rect.w - 1);
    const int16_t rel     = clampInt(x - _pad_rect.x, 0, usableW);
    return clampInt(_yaw_max - (static_cast<int32_t>(rel) * (_yaw_max - _yaw_min)) / usableW, _yaw_min, _yaw_max);
}

int16_t StackRemoteView::touchToPitch(int16_t y) const
{
    const int16_t usableH = atLeastOne(_pad_rect.h - 1);
    const int16_t rel     = clampInt(y - _pad_rect.y, 0, usableH);
    return clampInt(_pitch_max - (static_cast<int32_t>(rel) * (_pitch_max - _pitch_min)) / usableH, _pitch_min,
                    _pitch_max);
}

int16_t StackRemoteView::yawToX(int16_t yaw) const
{
    const int16_t usableW = atLeastOne(_pad_rect.w - 1);
    return _pad_rect.x + static_cast<int16_t>((static_cast<int32_t>(_yaw_max - yaw) * usableW) /
                                              (_yaw_max - _yaw_min));
}

int16_t StackRemoteView::pitchToY(int16_t pitch) const
{
    const int16_t usableH = atLeastOne(_pad_rect.h - 1);
    return _pad_rect.y + static_cast<int16_t>((static_cast<int32_t>(_pitch_max - pitch) * usableH) /
                                              (_pitch_max - _pitch_min));
}
