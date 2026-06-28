/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_stack_remote.h"

#include <assets/assets.h>
#include <hal/hal.h>
#include <mooncake_log.h>

AppStackRemote::AppStackRemote()
{
    setAppInfo().name = "StackRemote";
    setAppInfo().icon = (void*)&icon_stack_remote;

    static uint32_t theme_color = 0x58D8E8;
    setAppInfo().userData       = (void*)&theme_color;
}

void AppStackRemote::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppStackRemote::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    _key_manager = std::make_unique<input::KeyManager>();

    GetHAL().stopLvglUpdate();
    {
        LvglLockGuard lock;
    }

    _view = std::make_unique<view::StackRemoteView>();
    _view->init();
}

void AppStackRemote::onRunning()
{
    if (_key_manager && _key_manager->update() == input::KeyEvent::GoHome) {
        close();
        return;
    }

    if (_view) {
        _view->update();
    }
}

void AppStackRemote::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    _key_manager.reset();
    _view.reset();
    GetHAL().startLvglUpdate();
}
