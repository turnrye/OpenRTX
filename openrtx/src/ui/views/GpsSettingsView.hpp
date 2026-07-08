/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_GPSSETTINGSVIEW_HPP
#define ORTX_UI_GPSSETTINGSVIEW_HPP

#include <cstdint>
#include "core/View.hpp"
#include "layout/Flex.hpp"
#include "widgets/TopBar.hpp"
#include "widgets/List.hpp"
#include "core/state.h"

namespace ortxui
{

/**
 * The Settings > GPS screen (value-row style), mirroring the classic GPS
 * settings menu: GPS Enabled (On/Off), GPS Set Time (On/Off), and UTC Timezone
 * (in half-hour steps, shown as e.g. "+1.0"). ENTER opens edit; UP/DOWN toggles
 * a boolean or nudges the timezone; ENTER/ESC commits. All fields live in
 * state.settings (persisted at shutdown); none need an rtx resync.
 *
 * This is distinct from GpsView (the GPS *Position* home screen wired to the
 * main menu); the GPS on/off + set-time toggles live here, not in the
 * Accessibility checklist.
 */
class GpsSettingsView : public View
{
public:
    void build();
    void syncFromState(const state_t &s) override;
    NavIntent onEvent(const Event &e) override;

    Screen &screen() override
    {
        return screen_;
    }

private:
    enum Row : uint8_t {
        RowEnabled,
        RowSetTime,
        RowTimezone,
        RowCount,
    };

    /* Timezone bounds in half-hours: -12:00 .. +14:00. */
    static constexpr int8_t kTzMin = -24;
    static constexpr int8_t kTzMax = 28;

    void writeValueText(uint8_t row);
    void beginEdit();
    void endEdit();
    void adjust(int dir);
    void applyToggle(uint16_t row, bool on); //< flip a checkbox setting

    Screen screen_;
    Flex root_;
    TopBar topBar_;
    List list_;

    ListItem items_[RowCount] = {};
    char bufs_[RowCount][16] = {};
    bool editing_ = false;
    uint8_t editRow_ = 0;

    /* Change-gate mirrors. */
    uint8_t lastEnabled_ = 0xFFu;
    uint8_t lastSetTime_ = 0xFFu;
    int8_t lastTz_ = INT8_MIN;
};

} // namespace ortxui

#endif /* ORTX_UI_GPSSETTINGSVIEW_HPP */
