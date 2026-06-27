/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "view.h"

#include <assets/assets.h>
#include <hal/hal.h>
#include <array>
#include <cmath>

using namespace view;

namespace {

constexpr int _panel_size          = 466;
constexpr int _panel_center        = _panel_size / 2;
constexpr int _avatar_image_size   = 251;
constexpr int _center_dead_zone_px = 50;
constexpr float _rad_to_deg        = 57.29577951308232f;

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
    _current_direction = 4;

    auto& display = GetHAL().getDisplay();
    display.fillScreen(TFT_BLACK);

    if (loadImages()) {
        render();
    }
}

void GuruguruView::trackFace(int x, int y)
{
    setDirection(direction_from_point(x, y));
}

bool GuruguruView::loadImages()
{
    auto& display = GetHAL().getDisplay();

    for (std::size_t i = 0; i < _direction_images.size(); ++i) {
        auto canvas = std::make_unique<M5Canvas>(&display);
        canvas->setColorDepth(16);
        if (canvas->createSprite(_avatar_image_size, _avatar_image_size) == nullptr) {
            _direction_canvases = {};
            return false;
        }

        canvas->fillSprite(TFT_BLACK);
        const auto* image = _direction_images[i];
        if (!canvas->drawPng(image->data, static_cast<uint32_t>(image->data_size), 0, 0)) {
            _direction_canvases = {};
            return false;
        }
        _direction_canvases[i] = std::move(canvas);
    }

    _composite = std::make_unique<M5Canvas>(&display);
    _composite->setColorDepth(16);
    if (_composite->createSprite(display.width(), display.height()) == nullptr) {
        _direction_canvases = {};
        _composite.reset();
        return false;
    }

    return true;
}

void GuruguruView::render()
{
    if (!_composite || !_direction_canvases[_current_direction]) {
        return;
    }

    auto& display = GetHAL().getDisplay();
    const float zoom_x = static_cast<float>(display.width()) / _avatar_image_size;
    const float zoom_y = static_cast<float>(display.height()) / _avatar_image_size;

    _composite->fillSprite(TFT_BLACK);
    _direction_canvases[_current_direction]->pushRotateZoom(
        _composite.get(), display.width() / 2, display.height() / 2, 0.0f, zoom_x, zoom_y);

    display.startWrite();
    _composite->pushSprite(&display, 0, 0);
    display.endWrite();
}

void GuruguruView::setDirection(int direction)
{
    if (direction < 0 || direction >= static_cast<int>(_direction_images.size())) {
        return;
    }

    if (direction == _current_direction) {
        return;
    }

    _current_direction = direction;
    render();
}
