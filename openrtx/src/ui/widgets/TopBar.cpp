/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "widgets/TopBar.hpp"

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
}

} // namespace ortxui
