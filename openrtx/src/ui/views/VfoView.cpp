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

    const bool regular = (sizeClass() == SizeClass::Regular);
    const int16_t barH = regular ? 16 : 12;
    const int16_t smeterH = regular ? 8 : 6;
    const fontSize_t freqFont = regular ? FONT_SIZE_16PT : FONT_SIZE_12PT;
    const fontSize_t callFont = regular ? FONT_SIZE_8PT : FONT_SIZE_6PT;
    const fontSize_t barFont = FONT_SIZE_6PT;

    /* Root: a full-screen column of status bar / hero / meter / action bar. */
    root_.setArea({ 0, 0, (uint16_t)W, (uint16_t)H });
    root_.setAxis(Axis::Column);

    /* Status bar: [time | mode badge | battery], pinned to the top. */
    topBar_.setAxis(Axis::Row);
    topBar_.setJustify(Justify::SpaceBetween);
    topBar_.setAlign(Align::Stretch);
    topBar_.setBackground(Sem::Surface);
    topBar_.setPadding(4, 0);
    topBar_.setBasis(barH);

    time_.setFont(barFont);
    time_.setAlign(TEXT_ALIGN_LEFT);
    time_.setColor(Sem::OnSurface);
    time_.setBasis(34);

    mode_.setBasis(34);
    mode_.setAlignSelf(Align::Center);
    mode_.setArea({ 0, 0, 32, 12 }); /* natural size for cross-centring */

    battery_.setFont(barFont);
    battery_.setAlign(TEXT_ALIGN_RIGHT);
    battery_.setColor(Sem::OnSurface);
    battery_.setBasis(34);

    topBar_.addChild(&time_);
    topBar_.addChild(&mode_);
    topBar_.addChild(&battery_);

    /* Hero: frequency over callsign, centred in the growing middle region. */
    hero_.setAxis(Axis::Column);
    hero_.setJustify(Justify::Center);
    hero_.setAlign(Align::Stretch);
    hero_.setGap(2);
    hero_.setGrow(1);

    freq_.setFont(freqFont);
    freq_.setAlign(TEXT_ALIGN_CENTER);
    freq_.setColor(Sem::OnSurface);
    freq_.setBasis(gfx_getFontHeight(freqFont));

    callsign_.setFont(callFont);
    callsign_.setAlign(TEXT_ALIGN_CENTER);
    callsign_.setColor(Sem::OnSurfaceMuted);
    callsign_.setBasis(gfx_getFontHeight(callFont));

    hero_.addChild(&freq_);
    hero_.addChild(&callsign_);

    /* S-meter: full-width bar with side margins, above the action bar. */
    smeter_.setColors(Sem::SurfaceHigh, Sem::RxSuccess);
    smeter_.setValue(0.5f);
    smeter_.setBasis(smeterH);
    smeter_.setMargin(4);

    /* Action bar: [VFO | TONE], pinned to the bottom. */
    botBar_.setAxis(Axis::Row);
    botBar_.setJustify(Justify::SpaceBetween);
    botBar_.setAlign(Align::Stretch);
    botBar_.setBackground(Sem::Surface);
    botBar_.setPadding(4, 0);
    botBar_.setBasis(barH);

    leftAction_.setFont(barFont);
    leftAction_.setAlign(TEXT_ALIGN_LEFT);
    leftAction_.setColor(Sem::Primary);
    leftAction_.setText("VFO");
    leftAction_.setBasis(48);

    rightAction_.setFont(barFont);
    rightAction_.setAlign(TEXT_ALIGN_RIGHT);
    rightAction_.setColor(Sem::Primary);
    rightAction_.setText("TONE");
    rightAction_.setBasis(48);

    botBar_.addChild(&leftAction_);
    botBar_.addChild(&rightAction_);

    /* Assemble the column and lay the whole tree out once. */
    root_.addChild(&topBar_);
    root_.addChild(&hero_);
    root_.addChild(&smeter_);
    root_.addChild(&botBar_);
    screen_.addChild(&root_);
    root_.onLayout();

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
