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
    setAlign(Align::Center);
    setBackground(Sem::Surface);
    setBasis(barH);
    setPadding(4, 0);
    setGap(4);

    /* A left pad the size of the right cluster keeps the centred element near
     * the true middle while still leaving it the full width for long titles. */
    leftPad_.setText("");
    leftPad_.setBasis(regular ? 28 : 22);

    /* Centre: page title, or the clock when there is no title. */
    middle_.setFont(font);
    middle_.setAlign(TEXT_ALIGN_CENTER);
    middle_.setColor(Sem::OnSurface);
    middle_.setGrow(1);

    /* Right cluster: lock glyph (while locked) + battery as percentage or icon. */
    lock_.setFont(font);
    lock_.setAlign(TEXT_ALIGN_RIGHT);
    lock_.setColor(Sem::Accent);
    lock_.setText(SYMBOL_LOCK);
    lock_.setBasis(regular ? 12 : 10);
    lock_.setFlag(FLAG_HIDDEN, true);

    pct_.setFont(font);
    pct_.setAlign(TEXT_ALIGN_RIGHT);
    pct_.setColor(Sem::OnSurface);
    pct_.setText(pctBuf_);
    pct_.setBasis(regular ? 28 : 24);

    battery_.setBasis(22);
    battery_.setAlignSelf(Align::Center);
    battery_.setArea({ 0, 0, 22, 11 }); /* natural size for cross-centring */

    addChild(&leftPad_);
    addChild(&middle_);
    addChild(&lock_);
    addChild(&pct_);
    addChild(&battery_);

    titleStr_ = (title != nullptr) ? title : "";
    hasTitle_ = (titleStr_[0] != '\0');
    middle_.setText(hasTitle_ ? titleStr_ : clockBuf_);
}

void TopBar::setTitle(const char *t)
{
    titleStr_ = (t != nullptr) ? t : "";
    hasTitle_ = (titleStr_[0] != '\0');
    middle_.setText(hasTitle_ ? titleStr_ : clockBuf_);
    middle_.invalidate();
}

void TopBar::update(const state_t &s)
{
    /* The clock occupies the centre only when the screen has no title. */
    if (!hasTitle_ && (s.time.minute != lastMinute_)) {
        snprintf(clockBuf_, sizeof(clockBuf_), "%02u:%02u", s.time.hour,
                 s.time.minute);
        middle_.setText(clockBuf_);
        middle_.invalidate();
        lastMinute_ = s.time.minute;
    }

    if (s.charge != lastCharge_) {
        snprintf(pctBuf_, sizeof(pctBuf_), "%u%%", s.charge);
        pct_.invalidate();
        battery_.setCharge(s.charge);
        battery_.invalidate();
        lastCharge_ = s.charge;
    }

    bool relayout = false;

    /* Battery: percentage OR icon, per settings.showBatteryIcon. */
    const int8_t showIcon = s.settings.showBatteryIcon ? 1 : 0;
    if (showIcon != lastShowIcon_) {
        pct_.setFlag(FLAG_HIDDEN, showIcon != 0);
        battery_.setFlag(FLAG_HIDDEN, showIcon == 0);
        lastShowIcon_ = showIcon;
        relayout = true;
    }

    if (s.keypad_locked != lastLocked_) {
        lock_.setFlag(FLAG_HIDDEN, !s.keypad_locked);
        lastLocked_ = s.keypad_locked;
        relayout = true;
    }

    if (relayout) {
        onLayout();
        invalidate();
    }
}

} // namespace ortxui
