/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/TimeDateView.hpp"
#include "core/Event.hpp"
#include "interfaces/keyboard.h"
#include "interfaces/platform.h"
#include "hwconfig.h"

#include <cstdio>
#include <cstring>

namespace ortxui
{

namespace
{
/* dd/mm/yy hh:mm — seconds are fixed at 00, matching the classic editor. */
constexpr uint8_t kDigits = 10;

/* KEY_0..KEY_9 occupy bits 0..9, so a set digit bit's position is its value. */
constexpr uint32_t kDigitMask = 0x03FFu;
} // namespace

void TimeDateView::build()
{
    const int16_t W = CONFIG_SCREEN_WIDTH;
    const int16_t H = CONFIG_SCREEN_HEIGHT;
    const bool regular = (sizeClass() == SizeClass::Regular);

    root_.setArea({ 0, 0, (uint16_t)W, (uint16_t)H });
    root_.setAxis(Axis::Column);

    topBar_.init("Time & Date");
    setTopBar(&topBar_);

    /* Centred date-over-time stack with a small hint below. */
    body_.setAxis(Axis::Column);
    body_.setAlign(Align::Stretch);
    body_.setJustify(Justify::Center);
    body_.setGap(regular ? 6 : 2);
    body_.setGrow(1);

    const fontSize_t big = regular ? FONT_SIZE_12PT : FONT_SIZE_8PT;
    dateLabel_.setFont(big);
    dateLabel_.setAlign(TEXT_ALIGN_CENTER);
    dateLabel_.setColor(Sem::OnSurface);
    dateLabel_.setBasis(regular ? 26 : 16);

    timeLabel_.setFont(big);
    timeLabel_.setAlign(TEXT_ALIGN_CENTER);
    timeLabel_.setColor(Sem::OnSurface);
    timeLabel_.setBasis(regular ? 26 : 16);

    hint_.setFont(FONT_SIZE_6PT);
    hint_.setAlign(TEXT_ALIGN_CENTER);
    hint_.setColor(Sem::OnSurfaceMuted);
    hint_.setText("ENTER=set");
    hint_.setBasis(12);

    body_.addChild(&dateLabel_);
    body_.addChild(&timeLabel_);
    body_.addChild(&hint_);

    root_.addChild(&topBar_);
    root_.addChild(&body_);
    screen_.addChild(&root_);
    root_.onLayout();

    showDisplay(state, true);
    screen_.markAllDirty();
}

void TimeDateView::showDisplay(const state_t &s, bool force)
{
    const datetime_t t = utcToLocalTime(s.time, s.settings.utc_timezone);

    char combined[32];
    snprintf(combined, sizeof(combined), "%02d/%02d/%02d %02d:%02d:%02d",
             t.date, t.month, t.year, t.hour, t.minute, t.second);
    if (!force && (strncmp(combined, dispCache_, sizeof(dispCache_)) == 0))
        return;
    strncpy(dispCache_, combined, sizeof(dispCache_) - 1);
    dispCache_[sizeof(dispCache_) - 1] = '\0';

    snprintf(dateBuf_, sizeof(dateBuf_), "%02d/%02d/%02d", t.date, t.month,
             t.year);
    snprintf(timeBuf_, sizeof(timeBuf_), "%02d:%02d:%02d", t.hour, t.minute,
             t.second);
    dateLabel_.setText(dateBuf_);
    timeLabel_.setText(timeBuf_);
    dateLabel_.invalidate();
    timeLabel_.invalidate();
}

void TimeDateView::syncFromState(const state_t &s)
{
    View::syncFromState(s); /* refresh the shared top bar */
    if (!editing_)
        showDisplay(s, false);
}

void TimeDateView::onShow()
{
    /* Never resume mid-edit. */
    if (editing_) {
        editing_ = false;
        showDisplay(state, true);
        hint_.setText("ENTER=set");
        hint_.invalidate();
    }
}

void TimeDateView::announce()
{
    vpSay("Time and Date");
}

void TimeDateView::beginEdit()
{
    editing_ = true;
    pos_ = 0;
    memset(&edit_, 0, sizeof(edit_));
    /* Placeholders; seconds are not editable (fixed 00). */
    strcpy(dateBuf_, "__/__/__");
    strcpy(timeBuf_, "__:__:00");
    hint_.setText("dd/mm/yy hh:mm");
    hint_.invalidate();
    refreshEdit();
}

void TimeDateView::addDigit(uint8_t digit)
{
    if (pos_ >= kDigits)
        return;
    pos_++;

    /* Place the typed character into the display buffer, skipping the fixed
     * separators (classic _ui_drawSettingsTimeDateSet layout). */
    const char c = static_cast<char>('0' + digit);
    if (pos_ <= 6) {
        uint8_t idx = static_cast<uint8_t>(pos_ - 1);
        if (pos_ > 2)
            idx++; /* skip the first '/' */
        if (pos_ > 4)
            idx++; /* skip the second '/' */
        dateBuf_[idx] = c;
    } else {
        uint8_t idx = static_cast<uint8_t>(pos_ - 7);
        if (pos_ > 8)
            idx++; /* skip the ':' */
        timeBuf_[idx] = c;
    }

    /* Accumulate into the datetime (tens then ones per field). */
    switch (pos_) {
        case 1:
            edit_.date = static_cast<int8_t>(digit * 10);
            break;
        case 2:
            edit_.date = static_cast<int8_t>(edit_.date + digit);
            break;
        case 3:
            edit_.month = static_cast<int8_t>(digit * 10);
            break;
        case 4:
            edit_.month = static_cast<int8_t>(edit_.month + digit);
            break;
        case 5:
            edit_.year = static_cast<uint8_t>(digit * 10);
            break;
        case 6:
            edit_.year = static_cast<uint8_t>(edit_.year + digit);
            break;
        case 7:
            edit_.hour = static_cast<int8_t>(digit * 10);
            break;
        case 8:
            edit_.hour = static_cast<int8_t>(edit_.hour + digit);
            break;
        case 9:
            edit_.minute = static_cast<int8_t>(digit * 10);
            break;
        case 10:
            edit_.minute = static_cast<int8_t>(edit_.minute + digit);
            break;
        default:
            break;
    }
    refreshEdit();
}

void TimeDateView::refreshEdit()
{
    dateLabel_.setText(dateBuf_);
    timeLabel_.setText(timeBuf_);
    dateLabel_.invalidate();
    timeLabel_.invalidate();
}

void TimeDateView::commit()
{
    /* The user entered a LOCAL time; store the equivalent UTC. */
    const datetime_t utc = localTimeToUtc(edit_, state.settings.utc_timezone);
    platform_setTime(utc);
    state.time = utc;
    editing_ = false;
    hint_.setText("ENTER=set");
    hint_.invalidate();
    showDisplay(state, true);
}

NavIntent TimeDateView::onEvent(const Event &e)
{
    if (editing_) {
        if (e.kind != EvKind::Key)
            return NavIntent::none();
        if ((e.keys & KEY_ESC) != 0u) {
            editing_ = false;
            hint_.setText("ENTER=set");
            hint_.invalidate();
            showDisplay(state, true);
        } else if ((e.keys & KEY_ENTER) != 0u) {
            if (pos_ >= kDigits)
                commit(); /* only a complete entry is applied */
        } else if ((e.keys & kDigitMask) != 0u) {
            addDigit(static_cast<uint8_t>(__builtin_ctz(e.keys & kDigitMask)));
        }
        return NavIntent::none();
    }

    if (e.kind == EvKind::Key) {
        if ((e.keys & KEY_ESC) != 0u)
            return NavIntent::pop();
        if ((e.keys & KEY_ENTER) != 0u) {
            beginEdit();
            return NavIntent::none();
        }
    }

    return NavIntent::none();
}

} // namespace ortxui
