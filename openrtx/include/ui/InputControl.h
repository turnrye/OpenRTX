/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INPUT_CONTROL_H
#define INPUT_CONTROL_H

#include "ui/UIContext.h"
#include "core/event.h"

/**
 * Result returned by InputControl::handleKey().
 */
enum class InputResult
{
    Ongoing,    ///< Input still in progress; no state change
    Confirmed,  ///< User confirmed the entry (e.g., pressed ENTER)
    Cancelled,  ///< User cancelled (e.g., pressed ESC)
};

/**
 * Abstract base class for self-contained input controls.
 *
 * Input controls encapsulate a specific data-entry mode (text, frequency,
 * number, date/time) including their own buffers and cursor state.  A screen
 * owns one or more InputControl instances as members and delegates keypress
 * events to them while the control is active.
 *
 * All instances must be statically allocated (no heap usage).
 */
class InputControl
{
public:
    virtual ~InputControl() = default;

    /**
     * Reset the control to its initial state, clearing any buffered input.
     */
    virtual void reset() = 0;

    /**
     * Handle a single keypress.
     *
     * @param ctx    Shared UI context
     * @param event  The event to handle
     * @return InputResult indicating whether input is still in progress,
     *         confirmed, or cancelled.
     */
    virtual InputResult handleKey(UIContext& ctx, event_t event) = 0;

    /**
     * Render the control (overlay rectangle or inline field).
     *
     * @param ctx      Shared UI context
     * @param overlay  When true, draw a centered modal overlay rectangle.
     *                 When false, render inline at the current layout position.
     */
    virtual void draw(UIContext& ctx, bool overlay = true) = 0;
};

#endif // INPUT_CONTROL_H
