/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SCREEN_H
#define SCREEN_H

#include "ui/UIContext.h"
#include "core/event.h"

/**
 * Abstract base class for all UI screens.
 *
 * Subclasses implement handleInput() and draw(). Optionally override
 * onEntry()/onExit() for initialization and cleanup respectively.
 *
 * All instances must be statically allocated (no heap usage).
 */
class Screen
{
public:
    virtual ~Screen() = default;

    /**
     * Called once when this screen becomes the active screen.
     */
    virtual void onEntry(UIContext& ctx) { (void)ctx; }

    /**
     * Called once when this screen is deactivated.
     */
    virtual void onExit(UIContext& ctx) { (void)ctx; }

    /**
     * Handle a single input event.
     *
     * @param ctx       Shared UI context
     * @param event     The event to handle
     * @param sync_rtx  Set to true if radio state needs to be synchronized
     * @return true if the event was consumed
     */
    virtual bool handleInput(UIContext& ctx, event_t event, bool* sync_rtx) = 0;

    /**
     * Render the screen contents.
     *
     * @param ctx  Shared UI context
     */
    virtual void draw(UIContext& ctx) = 0;
};

#endif // SCREEN_H
