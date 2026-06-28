/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <M5GFX.h>
#include <cstdint>
#include <memory>

namespace view {

class StackRemoteView {
public:
    ~StackRemoteView();

    void init();
    void update();

private:
    struct Rect {
        int16_t x = 0;
        int16_t y = 0;
        int16_t w = 0;
        int16_t h = 0;

        bool contains(int16_t px, int16_t py) const
        {
            return px >= x && px < x + w && py >= y && py < y + h;
        }
    };

    enum class HitArea : uint8_t {
        None,
        Pad,
        Channel,
        Target,
        Speed,
        Mode,
        Laser,
        Home,
        Hold,
    };

    struct RemoteState {
        uint8_t channel      = 1;
        uint8_t targetId     = 0;
        int16_t yaw          = 0;
        int16_t pitch        = 450;
        int16_t speed        = 600;
        bool laser           = false;
        bool gyroMode        = false;
        bool holdOnRelease   = false;
        bool espNowReady     = false;
        bool lastSendOk      = false;
        uint32_t sentCount   = 0;
        int lastError        = 0;
    };

    struct TouchState {
        bool wasDown          = false;
        bool longApplied      = false;
        uint32_t downAt       = 0;
        HitArea activeArea    = HitArea::None;
    };

    std::unique_ptr<M5Canvas> _canvas;
    RemoteState _remote;
    TouchState _touch;

    Rect _channel_button;
    Rect _target_button;
    Rect _speed_button;
    Rect _mode_button;
    Rect _laser_button;
    Rect _pad_rect;
    Rect _home_button;
    Rect _hold_button;

    int16_t _screen_w = 0;
    int16_t _screen_h = 0;

    uint32_t _last_draw_at = 0;
    uint32_t _last_send_at = 0;
    bool _has_last_packet = false;
    uint8_t _last_target_id = 255;
    int16_t _last_yaw = 0;
    int16_t _last_pitch = 0;
    int16_t _last_speed = 0;
    bool _last_laser = false;
    uint32_t _last_gyro_at = 0;
    bool _gyro_calibrated = false;
    float _gyro_neutral_roll = 0.0f;
    float _gyro_neutral_pitch = 0.0f;
    float _gyro_roll = 0.0f;
    float _gyro_pitch = 0.0f;

    void buildLayout();
    void initEspNow();
    void deinitEspNow();
    void setWifiChannel(uint8_t channel);
    void sendPose(bool force = false);
    void homePose(bool force = true);
    void handleTouch();
    void handleControl(HitArea area, int direction);
    void updateGyroControl();
    bool readImuAngles(float& roll, float& pitch);
    void calibrateGyro();
    void drawIfNeeded(bool force = false);
    void drawUi();
    void drawButton(const Rect& rect, const char* text, bool active);
    void drawCenteredText(const Rect& rect, const char* text, uint16_t color);

    HitArea hitTest(int16_t x, int16_t y) const;
    int16_t touchToYaw(int16_t x) const;
    int16_t touchToPitch(int16_t y) const;
    int16_t yawToX(int16_t yaw) const;
    int16_t pitchToY(int16_t pitch) const;
};

}  // namespace view
