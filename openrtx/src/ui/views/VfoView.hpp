/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_VFOVIEW_HPP
#define ORTX_UI_VFOVIEW_HPP

#include "core/View.hpp"
#include "widgets/Widgets.hpp"
#include "layout/Flex.hpp"
#include "core/state.h"

namespace ortxui
{

/**
 * The VFO home screen, expressed as a composed widget tree.
 *
 * Widgets are direct members (static storage, no heap). build() wires the tree
 * and hands geometry to the Flex layout engine once (a column of status bar /
 * hero / meter / action bar, with the two bars as rows); no coordinates are
 * hand-placed. Fonts and fixed extents are chosen by SizeClass so the same
 * layout serves the mono and colour panels. syncFromState() pulls the handful
 * of radio fields it shows from the state snapshot each tick and invalidates
 * only the widgets whose value actually changed, so redraws are change-driven.
 *
 * As the navigation root, ENTER drills into the main menu (wired via setMenu())
 * and ESC has nowhere to go.
 */
class VfoView : public View
{
public:
    void build();

    /** Wire the menu opened when ENTER is pressed on the home screen. */
    void setMenu(View *menu)
    {
        menu_ = menu;
    }

    /** Pull displayed fields from the state snapshot (change-gated). */
    void syncFromState(const state_t &s) override;

    NavIntent onEvent(const Event &e) override;

    Screen &screen() override
    {
        return screen_;
    }

private:
    Screen screen_;

    /* Layout tree: a column of [status bar | hero | meter | action bar]; the
     * two bars are Flex rows that also paint their own surface background. */
    Flex root_;
    Flex topBar_;
    Flex hero_;
    Flex botBar_;

    Label time_;
    Chip mode_;
    Label battery_;
    Label freq_;
    Label callsign_;
    Bar smeter_;
    Label leftAction_;
    Label rightAction_;

    char timeBuf_[8] = { 0 };
    char battBuf_[8] = { 0 };
    char freqBuf_[16] = { 0 };
    char callsignCache_[16] = { 0 };

    uint32_t lastFreq_ = 0xFFFFFFFFu;
    uint8_t lastCharge_ = 0xFFu;
    uint8_t lastMode_ = 0xFFu;
    int16_t lastMinute_ = -1;

    View *menu_ = nullptr;
};

} // namespace ortxui

#endif /* ORTX_UI_VFOVIEW_HPP */
