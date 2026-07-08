/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_TOPBAR_HPP
#define ORTX_UI_TOPBAR_HPP

#include <cstdint>
#include "layout/Flex.hpp"
#include "widgets/Widgets.hpp"
#include "widgets/BatteryIcon.hpp"
#include "core/state.h"

namespace ortxui
{

/**
 * The shared screen header: a Surface-backed row with a centred element and a
 * right-aligned status cluster. The centre shows the page title, or — when the
 * screen has no title (the VFO) — the clock. The right cluster shows the
 * keypad-lock glyph (while locked, on every screen) and the battery as EITHER a
 * percentage or an icon, per settings.showBatteryIcon. A left pad balances the
 * cluster so the centre lands near the true middle while long titles still fit.
 * init() sizes the bar for the active SizeClass; update() refreshes the live
 * clock/battery/lock from the state snapshot, change-gated.
 */
class TopBar : public Flex
{
public:
    void init(const char *title);
    void setTitle(const char *t);
    const char *title() const
    {
        return titleStr_;
    }

    /** Refresh clock + battery + lock from the state snapshot (change-gated). */
    void update(const state_t &s);

private:
    Label lock_;   //< keypad-lock glyph at the far left (blank slot otherwise);
                   //  its reserved width also balances the right status cluster
    Label middle_; //< page title, or the clock when there is no title
    Label pct_;    //< battery percentage (when showBatteryIcon is false)
    BatteryIcon battery_; //< battery glyph (when showBatteryIcon is true)

    const char *titleStr_ = "";
    bool hasTitle_ = false;
    char clockBuf_[8] = { 0 };
    char pctBuf_[8] = { 0 };
    int16_t lastMinute_ = -1;
    uint8_t lastCharge_ = 0xFFu;
    bool lastLocked_ = false;
    int8_t lastShowIcon_ = -1; //< -1 forces the first battery-mode layout
};

} // namespace ortxui

#endif /* ORTX_UI_TOPBAR_HPP */
