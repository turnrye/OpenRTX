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
 * where each row shows a setting label with its current value low-right.
 * syncFromState() keeps the values live from the state snapshot. ENTER on a row
 * enters an edit mode (the value is wrapped in <...>); UP/right raises it,
 * DOWN/left lowers it, and ENTER/ESC commits. Edits are written straight into
 * state.settings (with the display side-effect applied immediately) and
 * persisted to flash at shutdown, mirroring the classic UI. The base View
 * handles ESC/scroll when not editing.
 */
class DisplayView : public View
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
        RowBrightness,
        RowContrast,
        RowSquelch,
        RowVox,
        RowTimer, //< display standby timeout (settings.display_timer)
        RowCount,
    };

    /** Inclusive value range and step per row. */
    struct Range {
        uint8_t min;
        uint8_t max;
        uint8_t step;
    };

    uint8_t curValue(uint8_t row) const;
    void applyValue(uint8_t row, uint8_t v); //< write settings + side-effect
    void writeValueText(uint8_t row, uint8_t v);
    void beginEdit();
    void endEdit();
    void adjust(int dir);

    Screen screen_;
    Flex root_;
    TopBar topBar_;
    List list_;

    ListItem items_[RowCount] = {};
    char bufs_[RowCount][12] = {};
    uint8_t last_[RowCount] = { 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu };
    bool editing_ = false;
    uint8_t editRow_ = 0;
};

} // namespace ortxui

#endif /* ORTX_UI_DISPLAYVIEW_HPP */
