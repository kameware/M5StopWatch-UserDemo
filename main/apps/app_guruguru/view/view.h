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
    std::unique_ptr<M5Canvas> _source_canvas;
    std::unique_ptr<M5Canvas> _composite;
    int _current_direction = -1;
    int _loaded_direction  = -1;

    bool initBuffers();
    bool loadDirection(int direction);
    void render();
    void setDirection(int direction);
};

}  // namespace view
