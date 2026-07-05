/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/VfoView.hpp"
#include "core/Event.hpp"
#include "rtx/rtx.h"
#include "hwconfig.h"
#include "interfaces/keyboard.h"

#include <cstdio>
#include <cstring>

namespace ortxui
{

/* Resolve a channel operating mode to its badge colour role and label. */
static Sem modeBadge(uint8_t mode, const char *&label)
{
    switch (mode) {
        case OPMODE_FM:
            label = "FM";
            return Sem::ModeFM;
        case OPMODE_DMR:
            label = "DMR";
            return Sem::ModeDMR;
        case OPMODE_M17:
            label = "M17";
            return Sem::ModeM17;
        default:
            label = "--";
            return Sem::OnSurfaceMuted;
    }
}

void VfoView::build()
{
    const int16_t W = CONFIG_SCREEN_WIDTH;
    const int16_t H = CONFIG_SCREEN_HEIGHT;
    const int16_t topH = 16;
    const int16_t botH = 16;

    /* Status + action bars */
    topBar_.setArea({ 0, 0, (uint16_t)W, (uint16_t)topH });
    topBar_.setColor(Sem::Surface);
    botBar_.setArea({ 0, (int16_t)(H - botH), (uint16_t)W, (uint16_t)botH });
    botBar_.setColor(Sem::Surface);

    /* Status bar contents (baseline ~11px so 6pt glyphs clear the top). */
    time_.setAnchor({ 4, 11 });
    time_.setFont(FONT_SIZE_6PT);
    time_.setAlign(TEXT_ALIGN_LEFT);
    time_.setColor(Sem::OnSurface);

    battery_.setAnchor({ 4, 11 }); /* RIGHT align: x is the right margin */
    battery_.setFont(FONT_SIZE_6PT);
    battery_.setAlign(TEXT_ALIGN_RIGHT);
    battery_.setColor(Sem::OnSurface);

    mode_.setArea({ (int16_t)((W / 2) - 16), 2, 32, 12 });

    /* Frequency hero + callsign sub-line */
    freq_.setAnchor({ (int16_t)(W / 2), (int16_t)(topH + 22) });
    freq_.setFont(FONT_SIZE_16PT);
    freq_.setAlign(TEXT_ALIGN_CENTER);
    freq_.setColor(Sem::OnSurface);

    callsign_.setAnchor({ (int16_t)(W / 2), (int16_t)(topH + 40) });
    callsign_.setFont(FONT_SIZE_8PT);
    callsign_.setAlign(TEXT_ALIGN_CENTER);
    callsign_.setColor(Sem::OnSurfaceMuted);

    /* S-meter bar */
    smeter_.setArea({ 4, (int16_t)(H - botH - 14), (uint16_t)(W - 8), 8 });
    smeter_.setColors(Sem::SurfaceHigh, Sem::RxSuccess);
    smeter_.setValue(0.5f);

    /* Action labels */
    leftAction_.setAnchor({ 4, (int16_t)(H - botH + 3) });
    leftAction_.setFont(FONT_SIZE_6PT);
    leftAction_.setAlign(TEXT_ALIGN_LEFT);
    leftAction_.setColor(Sem::Primary);
    leftAction_.setText("VFO");

    rightAction_.setAnchor({ 4, (int16_t)(H - botH + 3) });
    rightAction_.setFont(FONT_SIZE_6PT);
    rightAction_.setAlign(TEXT_ALIGN_RIGHT);
    rightAction_.setColor(Sem::Primary);
    rightAction_.setText("TONE");

    /* Assemble tree in paint order: bars first, then overlaid content. */
    screen_.addChild(&topBar_);
    screen_.addChild(&botBar_);
    screen_.addChild(&smeter_);
    screen_.addChild(&time_);
    screen_.addChild(&battery_);
    screen_.addChild(&mode_);
    screen_.addChild(&freq_);
    screen_.addChild(&callsign_);
    screen_.addChild(&leftAction_);
    screen_.addChild(&rightAction_);

    screen_.markAllDirty();
}

NavIntent VfoView::onEvent(const Event &e)
{
    /* ENTER drills into the main menu; the home view is the navigation root, so
     * ESC (handled by the Navigator popping) has nowhere to go. */
    if ((e.kind == EvKind::Key) && ((e.keys & KEY_ENTER) != 0u)
        && (menu_ != nullptr))
        return NavIntent::push(menu_);

    screen_.dispatch(e);
    return NavIntent::none();
}

void VfoView::syncFromState(const state_t &s)
{
    const uint32_t f = (uint32_t)s.channel.rx_frequency;
    if (f != lastFreq_) {
        snprintf(freqBuf_, sizeof(freqBuf_), "%lu.%05lu",
                 (unsigned long)(f / 1000000UL),
                 (unsigned long)((f % 1000000UL) / 10UL));
        freq_.setText(freqBuf_);
        freq_.invalidate();
        lastFreq_ = f;
    }

    if (s.charge != lastCharge_) {
        snprintf(battBuf_, sizeof(battBuf_), "%u%%", s.charge);
        battery_.setText(battBuf_);
        battery_.invalidate();
        lastCharge_ = s.charge;
    }

    if (s.channel.mode != lastMode_) {
        const char *label = "--";
        const Sem fill = modeBadge(s.channel.mode, label);
        mode_.setText(label);
        mode_.setColors(fill, Sem::OnPrimary);
        mode_.invalidate();
        lastMode_ = s.channel.mode;
    }

    if (s.time.minute != lastMinute_) {
        snprintf(timeBuf_, sizeof(timeBuf_), "%02u:%02u", s.time.hour,
                 s.time.minute);
        time_.setText(timeBuf_);
        time_.invalidate();
        lastMinute_ = s.time.minute;
    }

    if (strncmp(callsignCache_, s.settings.callsign, sizeof(callsignCache_))
        != 0) {
        strncpy(callsignCache_, s.settings.callsign,
                sizeof(callsignCache_) - 1);
        callsign_.setText(callsignCache_);
        callsign_.invalidate();
    }
}

} // namespace ortxui
