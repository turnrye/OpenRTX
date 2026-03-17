/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef UI_CONTEXT_H
#define UI_CONTEXT_H

#ifdef CONFIG_UI_MODULE17
#include "ui/ui_mod17.h"
#else
#include "ui/ui_default.h"
#endif

/**
 * Shared context passed to InputControl methods.
 *
 * Currently holds only a reference to the global layout.  As the UI
 * migrates to C++ Screen objects, this struct will grow to include
 * redraw flags, event queues, and other per-frame state so that
 * controls can be rendered and updated without reaching into globals.
 */
struct UIContext
{
    const layout_t& layout;

    explicit UIContext(const layout_t& l) : layout(l) {}
};

#endif // UI_CONTEXT_H
