/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/GpsSettingsView.hpp"
#include "core/Event.hpp"
#include "interfaces/keyboard.h"
#include "hwconfig.h"

#include <cstdio>

namespace ortxui
{

void GpsSettingsView::build()
{
    const int16_t W = CONFIG_SCREEN_WIDTH;
    const int16_t H = CONFIG_SCREEN_HEIGHT;
    const bool regular = (sizeClass() == SizeClass::Regular);

    static const char *const kLabels[RowCount] = {
        "GPS Enabled",
        "GPS Set Time",
        "UTC Timezone",
    };

    root_.setArea({ 0, 0, (uint16_t)W, (uint16_t)H });
    root_.setAxis(Axis::Column);

    topBar_.init("GPS");
    setTopBar(&topBar_);

    for (uint8_t i = 0; i < RowCount; i++)
        items_[i].label = kLabels[i];

    /* GPS Enabled / GPS Set Time are booleans -> checkboxes; the timezone is a
     * value row. */
    items_[RowEnabled].checkbox = true;
    items_[RowEnabled].checked = state.settings.gps_enabled;
    items_[RowSetTime].checkbox = true;
    items_[RowSetTime].checked = state.settings.gpsSetTime;
    items_[RowTimezone].value = bufs_[RowTimezone];
    writeValueText(RowTimezone);

    list_.setItems(items_, RowCount);
    list_.setRowHeight(regular ? 22 : 18); /* two-line value rows */
    list_.setFlag(FLAG_FOCUSABLE, true);
    list_.setGrow(1);

    root_.addChild(&topBar_);
    root_.addChild(&list_);
    screen_.addChild(&root_);
    root_.onLayout();
    screen_.focusFirst();

    screen_.markAllDirty();

    lastEnabled_ = state.settings.gps_enabled;
    lastSetTime_ = state.settings.gpsSetTime;
    lastTz_ = state.settings.utc_timezone;
}

void GpsSettingsView::writeValueText(uint8_t row)
{
    (void)row; /* only the timezone is a value row */
    const settings_t &st = state.settings; /* live source of truth */
    char inner[12];

    int8_t tz = st.utc_timezone;
    /* int8_t so the format-truncation analysis bounds the %d width. */
    int8_t hr = (int8_t)(tz / 2);
    int8_t mn = (int8_t)((tz % 2) * 5);
    char sign = ' ';
    if (tz > 0) {
        sign = '+';
    } else if (tz < 0) {
        sign = '-';
        hr = (int8_t)(-hr);
        mn = (int8_t)(-mn);
    }
    snprintf(inner, sizeof(inner), "%c%d.%d", sign, hr, mn);

    if (editing_) {
        snprintf(bufs_[RowTimezone], sizeof(bufs_[RowTimezone]), "<%s>", inner);
        vpSay(inner);
    } else {
        snprintf(bufs_[RowTimezone], sizeof(bufs_[RowTimezone]), "%s", inner);
    }
}

void GpsSettingsView::beginEdit()
{
    editRow_ = (uint8_t)list_.selected();
    editing_ = true;
    writeValueText(editRow_);
    list_.invalidate();
}

void GpsSettingsView::endEdit()
{
    editing_ = false;
    writeValueText(editRow_);
    list_.invalidate();
}

void GpsSettingsView::adjust(int dir)
{
    /* Only the timezone is adjustable (the booleans are checkboxes). */
    int tz = state.settings.utc_timezone + dir;
    if (tz < kTzMin)
        tz = kTzMin;
    if (tz > kTzMax)
        tz = kTzMax;
    state.settings.utc_timezone = (int8_t)tz;
    lastTz_ = state.settings.utc_timezone;

    writeValueText(RowTimezone);
    list_.invalidate();
}

void GpsSettingsView::applyToggle(uint16_t row, bool on)
{
    if (row == RowEnabled) {
        state.settings.gps_enabled = on;
        lastEnabled_ = on;
    } else if (row == RowSetTime) {
        state.settings.gpsSetTime = on;
        lastSetTime_ = on;
    }
}

void GpsSettingsView::syncFromState(const state_t &s)
{
    View::syncFromState(s); /* top bar */

    const settings_t &st = s.settings;
    bool changed = false;

    if (st.gps_enabled != lastEnabled_) {
        lastEnabled_ = st.gps_enabled;
        items_[RowEnabled].checked = st.gps_enabled;
        changed = true;
    }
    if (st.gpsSetTime != lastSetTime_) {
        lastSetTime_ = st.gpsSetTime;
        items_[RowSetTime].checked = st.gpsSetTime;
        changed = true;
    }
    if (!(editing_ && editRow_ == RowTimezone)
        && (st.utc_timezone != lastTz_)) {
        lastTz_ = st.utc_timezone;
        writeValueText(RowTimezone);
        changed = true;
    }

    if (changed)
        list_.invalidate();
}

NavIntent GpsSettingsView::onEvent(const Event &e)
{
    if (editing_) {
        int dir = 0;
        if (e.kind == EvKind::Encoder) {
            dir = e.encoder;
        } else if (e.kind == EvKind::Key) {
            if ((e.keys & (KEY_ENTER | KEY_ESC)) != 0u) {
                endEdit();
                return NavIntent::none();
            }
            if ((e.keys & (KEY_UP | KEY_RIGHT)) != 0u)
                dir = +1;
            else if ((e.keys & (KEY_DOWN | KEY_LEFT)) != 0u)
                dir = -1;
        }
        if (dir != 0)
            adjust(dir);
        return NavIntent::none();
    }

    if (e.kind == EvKind::Key) {
        if ((e.keys & KEY_ESC) != 0u)
            return NavIntent::pop();
        if ((e.keys & KEY_ENTER) != 0u) {
            ListItem *it = list_.selectedItem();
            if ((it != nullptr) && it->checkbox) {
                it->checked = !it->checked;
                applyToggle(list_.selected(), it->checked);
                list_.invalidate();
                vpSay(it->checked ? "On" : "Off"); /* toggle: no nav change */
            } else {
                beginEdit();                       /* the timezone value row */
            }
            return NavIntent::none();
        }
    }

    screen_.dispatch(e);
    return NavIntent::none();
}

} // namespace ortxui
