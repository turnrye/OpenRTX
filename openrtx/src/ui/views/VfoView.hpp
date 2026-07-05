/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_VFOVIEW_HPP
#define ORTX_UI_VFOVIEW_HPP

#include "core/Object.hpp"
#include "widgets/Widgets.hpp"
#include "core/state.h"

namespace ortxui
{

/**
 * The VFO home screen, expressed as a composed widget tree.
 *
 * Widgets are direct members (static storage, no heap). build() wires the
 * tree and fixed geometry once; syncFromState() pulls the handful of radio
 * fields it shows from the state snapshot each tick and invalidates only the
 * widgets whose value actually changed, so redraws are change-driven.
 *
 * Positions are hand-placed for now (160x128); the Flex/Grid layout engine and
 * responsive size classes replace the manual geometry in a later slice.
 */
class VfoView
{
public:
    void build();

    /** Pull displayed fields from the state snapshot (change-gated). */
    void syncFromState(const state_t &s);

    Screen &screen()
    {
        return screen_;
    }

private:
    Screen screen_;

    Panel topBar_;
    Panel botBar_;
    Label time_;
    Label battery_;
    Chip mode_;
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
};

} // namespace ortxui

#endif /* ORTX_UI_VFOVIEW_HPP */
