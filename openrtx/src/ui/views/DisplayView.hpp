/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_DISPLAYVIEW_HPP
#define ORTX_UI_DISPLAYVIEW_HPP

#include <cstdint>
#include "core/View.hpp"
#include "layout/Flex.hpp"
#include "widgets/TopBar.hpp"
#include "widgets/List.hpp"
#include "core/state.h"

namespace ortxui
{

/**
 * A settings screen in the "value row" style (mockup 2): a selectable list
 * where each row shows a setting label with its current value low-right. The
 * values are read-only for now (editing them is a later slice); syncFromState()
 * keeps them live from the state snapshot. The base View handles ESC/scroll.
 */
class DisplayView : public View
{
public:
    void build();
    void syncFromState(const state_t &s) override;

    Screen &screen() override
    {
        return screen_;
    }

private:
    enum Row : uint8_t {
        RowBrightness,
        RowContrast,
        RowSquelch,
        RowVox,
        RowCount,
    };

    Screen screen_;
    Flex root_;
    TopBar topBar_;
    List list_;

    ListItem items_[RowCount] = {};
    char bufs_[RowCount][12] = {};
    uint8_t last_[RowCount] = { 0xFFu, 0xFFu, 0xFFu, 0xFFu };
};

} // namespace ortxui

#endif /* ORTX_UI_DISPLAYVIEW_HPP */
