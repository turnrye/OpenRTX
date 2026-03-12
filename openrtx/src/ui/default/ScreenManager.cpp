/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ui/ScreenManager.h"
#include <string.h>

ScreenManager::ScreenManager()
    : active_(nullptr), activeId_(0xFF)
{
    memset(screens_, 0, sizeof(screens_));
}

void ScreenManager::registerScreen(uint8_t screenId, Screen* screen)
{
    if (screenId < MAX_SCREENS)
        screens_[screenId] = screen;
}

void ScreenManager::setActive(uint8_t screenId, UIContext& ctx)
{
    if (active_ != nullptr)
        active_->onExit(ctx);

    activeId_ = screenId;
    active_ = (screenId < MAX_SCREENS) ? screens_[screenId] : nullptr;

    if (active_ != nullptr)
        active_->onEntry(ctx);
}

bool ScreenManager::handleInput(UIContext& ctx, event_t event, bool* sync_rtx)
{
    if (active_ == nullptr)
        return false;
    return active_->handleInput(ctx, event, sync_rtx);
}

void ScreenManager::draw(UIContext& ctx)
{
    if (active_ != nullptr)
        active_->draw(ctx);
}
