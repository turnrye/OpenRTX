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
 * The shared screen header (mockups 1-6): a Surface-backed row with a
 * left-aligned title, and a right-aligned cluster of clock, battery percentage
 * and a battery-icon glyph. The title grows to push the clock/battery group to
 * the right edge, so it lands consistently whatever the title is (blank on the
 * VFO). init() sizes the bar for the active SizeClass; update() refreshes the
 * live clock and battery from the state snapshot, change-gated.
 */
class TopBar : public Flex
{
public:
    void init(const char *title);
    void setTitle(const char *t)
    {
        title_.setText(t);
    }

    /** Refresh clock + battery from the state snapshot (change-gated). */
    void update(const state_t &s);

private:
    Label title_;
    Label clock_;
    Label pct_;
    BatteryIcon battery_;

    char clockBuf_[8] = { 0 };
    char pctBuf_[8] = { 0 };
    int16_t lastMinute_ = -1;
    uint8_t lastCharge_ = 0xFFu;
};

} // namespace ortxui

#endif /* ORTX_UI_TOPBAR_HPP */
