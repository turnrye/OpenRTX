/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SCREEN_MANAGER_H
#define SCREEN_MANAGER_H

#include "ui/Screen.h"
#include "ui/UIContext.h"
#include "core/event.h"
#include <stdint.h>

// Maximum number of screen IDs (covers all uiScreen enum values)
#define MAX_SCREENS 32

/**
 * Registry and dispatcher for Screen instances.
 *
 * Screens are registered by ID (matching the uiScreen enum values).
 * All storage is static — no heap allocation.
 */
class ScreenManager
{
public:
    ScreenManager();

    /**
     * Register a screen instance for the given ID.
     * Safe to call before any screen becomes active.
     */
    void registerScreen(uint8_t screenId, Screen* screen);

    /**
     * Switch to the screen with the given ID.
     * Calls onExit() on the previously active screen and onEntry() on the new one.
     */
    void setActive(uint8_t screenId, UIContext& ctx);

    /**
     * Dispatch an input event to the currently active screen.
     * @return false if no screen is active.
     */
    bool handleInput(UIContext& ctx, event_t event, bool* sync_rtx);

    /**
     * Render the currently active screen.
     */
    void draw(UIContext& ctx);

    /**
     * Return the ID of the currently active screen.
     */
    uint8_t activeId() const { return activeId_; }

private:
    Screen*  screens_[MAX_SCREENS];
    Screen*  active_;
    uint8_t  activeId_;
};

#endif // SCREEN_MANAGER_H
