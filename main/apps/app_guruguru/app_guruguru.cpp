/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_guruguru.h"

#include <assets/assets.h>
#include <hal/hal.h>
#include <mooncake_log.h>

AppGuruguru::AppGuruguru()
{
    setAppInfo().name = "Guruguru";
    setAppInfo().icon = (void*)&icon_guruguru;
}

void AppGuruguru::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppGuruguru::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    _key_manager = std::make_unique<input::KeyManager>();

    GetHAL().stopLvglUpdate();
    {
        LvglLockGuard lock;
    }

    _view = std::make_unique<view::GuruguruView>();
    _view->init();
}

void AppGuruguru::onRunning()
{
    if (_key_manager && _key_manager->update() == input::KeyEvent::GoHome) {
        close();
        return;
    }

    if (_view) {
        auto point = GetHAL().getTouchPoint();
        if (point.num > 0) {
            _view->trackFace(point.x, point.y);
        }
    }
}

void AppGuruguru::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    _key_manager.reset();
    _view.reset();
    GetHAL().startLvglUpdate();
}
