/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/VfoView.hpp"
#include "core/Event.hpp"
#include "core/utils.h"
#include "rtx/rtx.h"
#include "interfaces/keyboard.h"
#include "hwconfig.h"

#include <cstdio>
#include <cstring>

namespace ortxui
{

void VfoView::build()
{
    const int16_t W = CONFIG_SCREEN_WIDTH;
    const int16_t H = CONFIG_SCREEN_HEIGHT;
    const bool regular = (sizeClass() == SizeClass::Regular);

    root_.setArea({ 0, 0, (uint16_t)W, (uint16_t)H });
    root_.setAxis(Axis::Column);

    /* Shared top bar with a blank title (VFO shows only the status cluster). */
    topBar_.init("");
    setTopBar(&topBar_);

    /* Frequency hero. */
    hero_.setBasis(regular ? 32 : 26);

    /* Channel line: index (muted) + name (blue). */
    chanRow_.setAxis(Axis::Row);
    chanRow_.setAlign(Align::Center);
    chanRow_.setJustify(Justify::Start);
    chanRow_.setPadding(4, 0);
    chanRow_.setGap(6);
    chanRow_.setBasis(regular ? 16 : 13);

    chanIdx_.setFont(FONT_SIZE_6PT);
    chanIdx_.setAlign(TEXT_ALIGN_LEFT);
    chanIdx_.setColor(Sem::OnSurfaceMuted);
    chanIdx_.setBasis(22);

    chanName_.setFont(regular ? FONT_SIZE_10PT : FONT_SIZE_8PT);
    chanName_.setAlign(TEXT_ALIGN_LEFT);
    chanName_.setColor(Sem::Primary); /* blue channel name */
    chanName_.setGrow(1);

    chanRow_.addChild(&chanIdx_);
    chanRow_.addChild(&chanName_);

    /* Signal meter. */
    meter_.setBasis(regular ? 14 : 12);
    meter_.setColor(Sem::RxSuccess);
    meter_.setLabel("RX");

    /* Open space below pushes the readout to the top. */
    spacer_.setGrow(1);

    root_.addChild(&topBar_);
    root_.addChild(&hero_);
    root_.addChild(&chanRow_);
    root_.addChild(&meter_);
    root_.addChild(&spacer_);
    screen_.addChild(&root_);
    root_.onLayout();

    screen_.markAllDirty();
}

void VfoView::syncMode(const channel_t &ch)
{
    const bool changed = (ch.mode != lastMode_)
                      || (ch.bandwidth != lastBandwidth_)
                      || (ch.fm.txToneEn != lastToneEn_)
                      || (ch.power != lastPower_);
    if (!changed)
        return;

    const char *m1 = "--";
    switch (ch.mode) {
        case OPMODE_FM:
            m1 = (ch.bandwidth == BW_25) ? "WFM" : "NFM";
            break;
        case OPMODE_DMR:
            m1 = "DMR";
            break;
        case OPMODE_M17:
            m1 = "M17";
            break;
        default:
            break;
    }

    const char *m2 = ((ch.mode == OPMODE_FM) && (ch.fm.txToneEn != 0u)) ?
                         "CCS" :
                         "";
    const char *m3 = (ch.power >= 2000u) ? "H" : "L";

    hero_.setMode(m1, m2, m3);
    hero_.invalidate();

    lastMode_ = ch.mode;
    lastBandwidth_ = ch.bandwidth;
    lastToneEn_ = ch.fm.txToneEn;
    lastPower_ = ch.power;
}

void VfoView::syncMeter(const state_t &s)
{
    /* Live TX/RX comes from the rtx module (state.rtxStatus is only the
     * requested mode); this matches how the classic UI reads it. */
    const uint8_t status = rtx_getStatus()->opStatus;
    const bool changed = (status != lastStatus_) || (s.rssi != lastRssi_);
    if (!changed)
        return;

    if (status == TX) {
        meter_.setLabel("TX");
        meter_.setColor(Sem::TxDanger);
        meter_.setShowScale(false);
        meter_.setLevel(9, 9); /* solid power bar */

        const uint32_t mw = s.channel.power;
        if (mw >= 1000u)
            snprintf(readoutBuf_, sizeof(readoutBuf_), "%lu.%luW",
                     (unsigned long)(mw / 1000u),
                     (unsigned long)((mw % 1000u) / 100u));
        else
            snprintf(readoutBuf_, sizeof(readoutBuf_), "%lumW",
                     (unsigned long)mw);
    } else {
        const uint8_t level = rssiToSlevel(s.rssi);
        const uint8_t filled = (level > 9u) ? 9u : level;

        meter_.setLabel("RX");
        meter_.setColor(Sem::RxSuccess);
        meter_.setShowScale(true);
        meter_.setLevel(filled, 9);

        if (level > 9u)
            snprintf(readoutBuf_, sizeof(readoutBuf_), "+%u",
                     (unsigned)((level - 9u) * 10u));
        else
            snprintf(readoutBuf_, sizeof(readoutBuf_), "S%u", (unsigned)level);
    }

    meter_.setReadout(readoutBuf_);
    meter_.invalidate();

    lastStatus_ = status;
    lastRssi_ = s.rssi;
}

void VfoView::syncFromState(const state_t &s)
{
    View::syncFromState(s); /* shared top bar */

    const channel_t &ch = s.channel;

    if (ch.rx_frequency != lastFreq_) {
        hero_.setFreq((uint32_t)ch.rx_frequency);
        hero_.invalidate();
        lastFreq_ = (uint32_t)ch.rx_frequency;
    }

    syncMode(ch);

    /* Channel line: in VFO (tuning) mode there is no channel, so show a "VFO"
     * label and no index; in memory mode show the 1-based index + name. */
    const bool chChanged =
        (s.tuner_mode != lastTuner_) || (s.channel_index != lastIdx_)
        || (strncmp(nameCache_, ch.name, sizeof(nameCache_)) != 0);
    if (chChanged) {
        strncpy(nameCache_, ch.name, sizeof(nameCache_) - 1);
        nameCache_[sizeof(nameCache_) - 1] = '\0';

        if (s.tuner_mode == VFO) {
            chanIdx_.setText("");
            chanName_.setText("VFO");
        } else {
            snprintf(idxBuf_, sizeof(idxBuf_), "%03u", s.channel_index + 1);
            chanIdx_.setText(idxBuf_);
            chanName_.setText((nameCache_[0] != '\0') ? nameCache_ : "---");
        }
        chanIdx_.invalidate();
        chanName_.invalidate();
        lastTuner_ = s.tuner_mode;
        lastIdx_ = s.channel_index;
    }

    syncMeter(s);
}

NavIntent VfoView::onEvent(const Event &e)
{
    if ((e.kind == EvKind::Key) && ((e.keys & KEY_ENTER) != 0u)
        && (menu_ != nullptr))
        return NavIntent::push(menu_);

    screen_.dispatch(e);
    return NavIntent::none();
}

} // namespace ortxui
