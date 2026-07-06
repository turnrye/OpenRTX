/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_VFOVIEW_HPP
#define ORTX_UI_VFOVIEW_HPP

#include "core/View.hpp"
#include "layout/Flex.hpp"
#include "widgets/Widgets.hpp"
#include "widgets/TopBar.hpp"
#include "widgets/FreqHero.hpp"
#include "widgets/DotMeter.hpp"
#include "core/state.h"

namespace ortxui
{

/**
 * The VFO home screen (mockups 5-6): a left-weighted instrument readout.
 *
 * A Flex column of shared top bar / frequency hero (big freq + sub-digits +
 * mode-tone-power stack) / channel line (index + name in blue) / signal meter /
 * open space. Widgets are direct members in static storage. build() wires the
 * tree and lays it out once; syncFromState() pulls the shown radio fields from
 * the state snapshot change-gated, and drives the meter from the RX/TX status
 * (green RX with an S-scale, orange TX as a solid power bar).
 *
 * As the navigation root, ENTER drills into the main menu (wired via setMenu());
 * ESC has nowhere to go.
 */
class VfoView : public View
{
public:
    void build();

    void setMenu(View *menu)
    {
        menu_ = menu;
    }

    void syncFromState(const state_t &s) override;
    NavIntent onEvent(const Event &e) override;

    Screen &screen() override
    {
        return screen_;
    }

private:
    void syncMode(const channel_t &ch);
    void syncMeter(const state_t &s);

    Screen screen_;

    Flex root_;
    TopBar topBar_;
    FreqHero hero_;
    Flex chanRow_;
    Label chanIdx_;
    Label chanName_;
    DotMeter meter_;
    Flex spacer_;

    char idxBuf_[8] = { 0 };
    char nameCache_[16] = { 0 };
    char readoutBuf_[12] = { 0 };

    uint32_t lastFreq_ = 0xFFFFFFFFu;
    uint16_t lastIdx_ = 0xFFFFu;
    uint8_t lastTuner_ = 0xFFu;
    uint8_t lastMode_ = 0xFFu;
    uint8_t lastBandwidth_ = 0xFFu;
    uint8_t lastToneEn_ = 0xFFu;
    uint32_t lastPower_ = 0xFFFFFFFFu;
    uint8_t lastStatus_ = 0xFFu;
    int32_t lastRssi_ = INT32_MIN;

    View *menu_ = nullptr;
};

} // namespace ortxui

#endif /* ORTX_UI_VFOVIEW_HPP */
