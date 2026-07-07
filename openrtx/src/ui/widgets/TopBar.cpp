/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "widgets/TopBar.hpp"
#include "core/graphics.h"

#include <cstdio>

namespace ortxui
{

void TopBar::init(const char *title)
{
    const bool regular = (sizeClass() == SizeClass::Regular);
    const int16_t barH = regular ? 16 : 12;
    const fontSize_t font = FONT_SIZE_6PT;

    setAxis(Axis::Row);
    setAlign(Align::Stretch);
    setBackground(Sem::Surface);
    setBasis(barH);
    setPadding(4, 0);
    setGap(6);

    /* Title takes the left and grows, pushing the status cluster to the right. */
    title_.setFont(font);
    title_.setAlign(TEXT_ALIGN_LEFT);
    title_.setColor(Sem::OnSurface);
    title_.setText(title);
    title_.setGrow(1);

    /* Keypad-lock indicator: sits between the title and the clock cluster,
     * hidden until the keypad is locked. Shown on every screen (all views
     * share this bar), so the lock state is always visible. */
    lock_.setFont(font);
    lock_.setAlign(TEXT_ALIGN_RIGHT);
    lock_.setColor(Sem::Accent);
    lock_.setText(SYMBOL_LOCK);
    lock_.setBasis(regular ? 12 : 10);
    lock_.setFlag(FLAG_HIDDEN, true);

    clock_.setFont(font);
    clock_.setAlign(TEXT_ALIGN_RIGHT);
    clock_.setColor(Sem::OnSurface);
    clock_.setBasis(34);

    pct_.setFont(font);
    pct_.setAlign(TEXT_ALIGN_RIGHT);
    pct_.setColor(Sem::OnSurface);
    pct_.setBasis(32);

    battery_.setBasis(22);
    battery_.setAlignSelf(Align::Center);
    battery_.setArea({ 0, 0, 22, 11 }); /* natural size for cross-centring */

    addChild(&title_);
    addChild(&lock_);
    addChild(&clock_);
    addChild(&pct_);
    addChild(&battery_);
}

void TopBar::update(const state_t &s)
{
    if (s.time.minute != lastMinute_) {
        snprintf(clockBuf_, sizeof(clockBuf_), "%02u:%02u", s.time.hour,
                 s.time.minute);
        clock_.setText(clockBuf_);
        clock_.invalidate();
        lastMinute_ = s.time.minute;
    }

    if (s.charge != lastCharge_) {
        snprintf(pctBuf_, sizeof(pctBuf_), "%u%%", s.charge);
        pct_.setText(pctBuf_);
        pct_.invalidate();
        battery_.setCharge(s.charge);
        battery_.invalidate();
        lastCharge_ = s.charge;
    }

    if (s.keypad_locked != lastLocked_) {
        lock_.setFlag(FLAG_HIDDEN, !s.keypad_locked);
        onLayout(); /* re-flow the row now the lock glyph appeared/vanished */
        invalidate();
        lastLocked_ = s.keypad_locked;
    }
}

} // namespace ortxui
