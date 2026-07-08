/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_RADIOVIEW_HPP
#define ORTX_UI_RADIOVIEW_HPP

#include <cstdint>
#include "core/View.hpp"
#include "layout/Flex.hpp"
#include "widgets/TopBar.hpp"
#include "widgets/List.hpp"
#include "core/state.h"

namespace ortxui
{

/**
 * The Settings > Radio screen (value-row style), mirroring the classic Radio
 * menu for the current channel:
 *  - Offset: the TX/RX split. ENTER opens a keypad entry (value shown as
 *    <N kHz>); digit keys append, an arrow backspaces, ENTER applies it as
 *    tx = rx + offset, ESC cancels.
 *  - Direction: '+' or '-'. ENTER opens edit; UP/DOWN flips the split sign.
 *  - Step: the tuning step; ENTER opens edit; UP/DOWN cycles freq_steps.
 *
 * Offset and Direction change tx_frequency, so they request an rtx resync;
 * Step only changes the tuning increment (state.step_index) and does not.
 */
class RadioView : public View
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
        RowOffset,
        RowDirection,
        RowStep,
        RowCount,
    };

    void writeValueText(uint8_t row);
    void beginEdit();
    void endEdit();
    void adjust(int dir); //< cycle edit (Direction/Step)
    void offsetDigit(uint8_t d);
    void offsetBackspace();
    void offsetApply();

    Screen screen_;
    Flex root_;
    TopBar topBar_;
    List list_;

    ListItem items_[RowCount] = {};
    char bufs_[RowCount][20] = {};
    bool editing_ = false;
    uint8_t editRow_ = 0;
    uint32_t offsetEntry_ = 0; //< keypad accumulator, in kHz

    /* Change-gate mirrors. */
    uint32_t lastTx_ = 0xFFFFFFFFu;
    uint32_t lastRx_ = 0xFFFFFFFFu;
    uint8_t lastStep_ = 0xFFu;
};

} // namespace ortxui

#endif /* ORTX_UI_RADIOVIEW_HPP */
