/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_CHECKLISTVIEW_HPP
#define ORTX_UI_CHECKLISTVIEW_HPP

#include <cstdint>
#include "core/View.hpp"
#include "layout/Flex.hpp"
#include "widgets/TopBar.hpp"
#include "widgets/List.hpp"
#include "core/state.h"

namespace ortxui
{

/**
 * A multi-toggle settings screen in the "checklist" style (mockup 4): a
 * selectable list of labelled checkboxes. ENTER toggles the selected row's
 * tick. The boxes are seeded from the matching state.settings flags on first
 * sync; toggling is local for now (persisting back to settings is a later
 * slice). The base View handles ESC/scroll.
 */
class ChecklistView : public View
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
        RowGps,
        RowPhonetic,
        RowLatch,
        RowGpsTime,
        RowBatteryIcon,
        RowCount,
    };

    Screen screen_;
    Flex root_;
    TopBar topBar_;
    List list_;

    ListItem items_[RowCount] = {};
    bool seeded_ = false;
};

} // namespace ortxui

#endif /* ORTX_UI_CHECKLISTVIEW_HPP */
