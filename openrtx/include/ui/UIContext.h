/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef UI_CONTEXT_H
#define UI_CONTEXT_H

#include "ui/ui_default.h"

struct UIContext
{
    layout_t   layout;
    ui_state_t ui_state;
    bool       layout_ready   = false;
    bool       redraw_needed  = true;
    bool       macro_menu     = false;
    bool       macro_latched  = false;
    bool       standby        = false;
    long long  last_event_tick = 0;

    // Event queue
    uint8_t evQueue_rdPos = 0;
    uint8_t evQueue_wrPos = 0;
    event_t evQueue[MAX_NUM_EVENTS];
};

#endif // UI_CONTEXT_H
