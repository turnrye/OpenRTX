/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/DisplayView.hpp"
#include "hwconfig.h"

#include <cstdio>

namespace ortxui
{

void DisplayView::build()
{
    const int16_t W = CONFIG_SCREEN_WIDTH;
    const int16_t H = CONFIG_SCREEN_HEIGHT;
    const bool regular = (sizeClass() == SizeClass::Regular);

    static const char *const kLabels[RowCount] = {
        "Brightness",
        "Contrast",
        "Squelch",
        "Vox",
    };

    root_.setArea({ 0, 0, (uint16_t)W, (uint16_t)H });
    root_.setAxis(Axis::Column);

    topBar_.init("Display");
    setTopBar(&topBar_);

    for (uint8_t i = 0; i < RowCount; i++) {
        items_[i].label = kLabels[i];
        items_[i].value = bufs_[i];
    }

    list_.setItems(items_, RowCount);
    list_.setRowHeight(regular ? 22 : 18); /* taller two-line value rows */
    list_.setFlag(FLAG_FOCUSABLE, true);
    list_.setGrow(1);

    root_.addChild(&topBar_);
    root_.addChild(&list_);
    screen_.addChild(&root_);
    root_.onLayout();
    screen_.focusFirst();

    screen_.markAllDirty();
}

void DisplayView::syncFromState(const state_t &s)
{
    View::syncFromState(s); /* top bar */

    const settings_t &st = s.settings;
    const uint8_t vals[RowCount] = { st.brightness, st.contrast, st.sqlLevel,
                                     st.voxLevel };
    bool changed = false;

    for (uint8_t i = 0; i < RowCount; i++) {
        if (vals[i] == last_[i])
            continue;

        if ((i == RowVox) && (vals[i] == 0u))
            snprintf(bufs_[i], sizeof(bufs_[i]), "Off");
        else if (i == RowSquelch)
            snprintf(bufs_[i], sizeof(bufs_[i]), "S%u", vals[i]);
        else
            snprintf(bufs_[i], sizeof(bufs_[i]), "%u", vals[i]);

        last_[i] = vals[i];
        changed = true;
    }

    if (changed)
        list_.invalidate();
}

} // namespace ortxui
