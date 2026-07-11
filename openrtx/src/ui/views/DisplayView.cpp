/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/DisplayView.hpp"
#include "core/Event.hpp"
#include "core/graphics.h"
#include "interfaces/keyboard.h"
#include "interfaces/display.h"
#include "hwconfig.h"

#include <cstdio>

namespace ortxui
{

void DisplayView::build()
{
    const int16_t W = CONFIG_SCREEN_WIDTH;
    const int16_t H = CONFIG_SCREEN_HEIGHT;

    static const char *const kLabels[RowCount] = {
        "Brightness", "Contrast", "Squelch", "Vox", "Timer", "Battery",
#ifdef CONFIG_PIX_FMT_RGB565
        "Theme",      "Text",
#endif
    };

    root_.setArea({ 0, 0, (uint16_t)W, (uint16_t)H });
    root_.setAxis(Axis::Column);

    topBar_.init("Display");
    setTopBar(&topBar_);

    for (uint8_t i = 0; i < RowCount; i++) {
        items_[i].label = kLabels[i];
        items_[i].value = bufs_[i];
        /* Seed from the live settings so no row starts blank (last_'s 0xFF
         * sentinel would otherwise alias a real value like contrast = 255). */
        const uint8_t v = curValue(i);
        writeValueText(i, v);
        last_[i] = v;
    }

    list_.setItems(items_, RowCount);
    list_.setContentPad(4);
    list_.setFlag(FLAG_FOCUSABLE, true);
    list_.setGrow(1);

    root_.addChild(&topBar_);
    root_.addChild(&list_);
    screen_.addChild(&root_);
    root_.onLayout();
    screen_.focusFirst();

    screen_.markAllDirty();
}

uint8_t DisplayView::curValue(uint8_t row) const
{
    switch (row) {
        case RowBrightness:
            return state.settings.brightness;
        case RowContrast:
            return state.settings.contrast;
        case RowSquelch:
            return state.settings.sqlLevel;
        case RowVox:
            return state.settings.voxLevel;
        case RowTimer:
            return state.settings.display_timer;
        case RowBattery:
            return state.settings.showBatteryIcon ? 1u : 0u;
#ifdef CONFIG_PIX_FMT_RGB565
        case RowTheme:
            return state.settings.highContrast ? 1u : 0u;
        case RowText:
            return state.settings.crispText ? 1u : 0u;
#endif
        default:
            return 0u;
    }
}

void DisplayView::applyValue(uint8_t row, uint8_t v)
{
    /* Write straight into the live settings and apply the display change now,
     * as the classic UI does; flash persistence happens at shutdown. */
    switch (row) {
        case RowBrightness:
            state.settings.brightness = v;
            display_setBacklightLevel(v);
            break;
        case RowContrast:
            state.settings.contrast = v;
            display_setContrast(v);
            break;
        case RowSquelch:
            state.settings.sqlLevel = v;
            break;
        case RowVox:
            state.settings.voxLevel = v;
            break;
        case RowTimer:
            /* No immediate side-effect: the standby loop reads display_timer
             * live each tick. */
            state.settings.display_timer = v;
            break;
        case RowBattery:
            state.settings.showBatteryIcon = (v != 0u);
            break;
#ifdef CONFIG_PIX_FMT_RGB565
        case RowTheme:
            /* themeColor() reads this live; repaint the whole screen so the new
             * palette takes effect immediately. */
            state.settings.highContrast = (v != 0u);
            screen_.markAllDirty();
            break;
        case RowText:
            state.settings.crispText = (v != 0u);
            gfx_setFontMono(v != 0u);
            screen_.markAllDirty();
            break;
#endif
        default:
            break;
    }
}

void DisplayView::writeValueText(uint8_t row, uint8_t v)
{
    /* Standby-timer labels, mirroring the classic display_timer_values table. */
    static const char *const kTimerLabels[] = {
        "Off",    "5 s",    "10 s",   "15 s",   "20 s",  "25 s",
        "30 s",   "1 min",  "2 min",  "3 min",  "4 min", "5 min",
        "15 min", "30 min", "45 min", "1 hour",
    };

    char inner[16];
    if ((row == RowVox) && (v == 0u))
        snprintf(inner, sizeof(inner), "Off");
    else if (row == RowSquelch)
        snprintf(inner, sizeof(inner), "S%u", v);
    else if (row == RowTimer)
        snprintf(inner, sizeof(inner), "%s", kTimerLabels[(v < 16u) ? v : 0u]);
    else if (row == RowBattery)
        snprintf(inner, sizeof(inner), "%s", v ? "Icon" : "Percent");
#ifdef CONFIG_PIX_FMT_RGB565
    else if (row == RowTheme)
        snprintf(inner, sizeof(inner), "%s", v ? "High Contrast" : "Standard");
    else if (row == RowText)
        snprintf(inner, sizeof(inner), "%s", v ? "Crisp" : "Smooth");
#endif
    else
        snprintf(inner, sizeof(inner), "%u", v);

    /* In edit mode the active row's value is bracketed to signal it is live, and
     * spoken so the change is heard per step (label was announced on nav). */
    if (editing_ && (row == editRow_)) {
        snprintf(bufs_[row], sizeof(bufs_[row]), "<%s>", inner);
        vpSay(inner);
    } else {
        snprintf(bufs_[row], sizeof(bufs_[row]), "%s", inner);
    }
}

void DisplayView::beginEdit()
{
    editRow_ = (uint8_t)list_.selected();
    editing_ = true;
    writeValueText(editRow_, curValue(editRow_));
    list_.invalidate();
}

void DisplayView::endEdit()
{
    editing_ = false;
    writeValueText(editRow_, curValue(editRow_));
    list_.invalidate();
}

void DisplayView::adjust(int dir)
{
    static const Range kRanges[RowCount] = {
        { 5u, 100u, 5u }, //< Brightness
        { 0u, 255u, 4u }, //< Contrast
        { 0u, 15u, 1u },  //< Squelch
        { 0u, 10u, 1u },  //< Vox
        { 0u, 15u, 1u },  //< Timer (TIMER_OFF..TIMER_1H)
        { 0u, 1u, 1u },   //< Battery (Percent / Icon)
#ifdef CONFIG_PIX_FMT_RGB565
        { 0u, 1u, 1u },   //< Theme (Standard / High Contrast)
        { 0u, 1u, 1u },   //< Text (Smooth / Crisp)
#endif
    };
    const Range &r = kRanges[editRow_];

    int v = (int)curValue(editRow_) + dir * (int)r.step;
    if (v < (int)r.min)
        v = r.min;
    if (v > (int)r.max)
        v = r.max;

    applyValue(editRow_, (uint8_t)v);
    last_[editRow_] = (uint8_t)v; /* keep syncFromState from reformatting */
    writeValueText(editRow_, (uint8_t)v);
    list_.invalidate();
}

void DisplayView::syncFromState(const state_t &s)
{
    View::syncFromState(s); /* top bar */

    const settings_t &st = s.settings;
    const uint8_t vals[RowCount] = {
        st.brightness,
        st.contrast,
        st.sqlLevel,
        st.voxLevel,
        (uint8_t)st.display_timer,
        (uint8_t)(st.showBatteryIcon ? 1u : 0u),
#ifdef CONFIG_PIX_FMT_RGB565
        (uint8_t)(st.highContrast ? 1u : 0u),
        (uint8_t)(st.crispText ? 1u : 0u),
#endif
    };
    bool changed = false;

    for (uint8_t i = 0; i < RowCount; i++) {
        /* Don't clobber the row the user is actively editing. */
        if (editing_ && (i == editRow_))
            continue;
        if (vals[i] == last_[i])
            continue;

        writeValueText(i, vals[i]);
        last_[i] = vals[i];
        changed = true;
    }

    if (changed)
        list_.invalidate();
}

NavIntent DisplayView::onEvent(const Event &e)
{
    if (editing_) {
        int dir = 0;
        if (e.kind == EvKind::Encoder) {
            dir = e.encoder;
        } else if (e.kind == EvKind::Key) {
            if ((e.keys & (KEY_ENTER | KEY_ESC)) != 0u) {
                endEdit();
                return NavIntent::none();
            }
            if ((e.keys & (KEY_UP | KEY_RIGHT)) != 0u)
                dir = +1;
            else if ((e.keys & (KEY_DOWN | KEY_LEFT)) != 0u)
                dir = -1;
        }
        if (dir != 0)
            adjust(dir);
        return NavIntent::none(); /* swallow all input while editing */
    }

    if (e.kind == EvKind::Key) {
        if ((e.keys & KEY_ESC) != 0u)
            return NavIntent::pop();
        if ((e.keys & KEY_ENTER) != 0u) {
            beginEdit();
            return NavIntent::none();
        }
    }

    screen_.dispatch(e);
    return NavIntent::none();
}

} // namespace ortxui
