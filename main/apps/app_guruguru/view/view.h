/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <M5GFX.h>
#include <array>
#include <memory>

namespace view {

class GuruguruView {
public:
    void init();
    void trackFace(int x, int y);

private:
    std::array<std::unique_ptr<M5Canvas>, 9> _direction_canvases;
    std::unique_ptr<M5Canvas> _composite;
    int _current_direction = 4;

    bool loadImages();
    void render();
    void setDirection(int direction);
};

}  // namespace view
