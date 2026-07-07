/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_M17VIEW_HPP
#define ORTX_UI_M17VIEW_HPP

#include <cstdint>
#include "core/View.hpp"
#include "layout/Flex.hpp"
#include "widgets/TopBar.hpp"
#include "widgets/List.hpp"
#include "widgets/TextInput.hpp"
#include "core/state.h"

namespace ortxui
{

class TextInputView;

/**
 * The Settings > M17 screen (value-row style), mirroring the classic M17 menu:
 *  - Callsign: edited in place with a deterministic cursor cycle — ENTER opens
 *    edit, the cursor character is shown in brackets (e.g. "W1[A]W"), UP/DOWN
 *    cycles it through the callsign charset, LEFT/RIGHT moves the cursor
 *    (extending with a space at the end), ENTER confirms, ESC cancels. (The
 *    classic UI uses T9 multi-tap, which is timing-dependent and not golden-
 *    testable; the cursor cycle is knob-friendly and reproducible.)
 *  - Meta Txt: shown read-only for now (long free text wants a richer editor).
 *  - CAN: 0-15, cycled with UP/DOWN.
 *  - CAN RX Check: On/Off, toggled with UP/DOWN.
 *
 * All fields live in state.settings (persisted at shutdown); none need an rtx
 * resync. The base View handles ESC/scroll when not editing.
 */
class M17View : public View
{
public:
    void build();
    void syncFromState(const state_t &s) override;
    NavIntent onEvent(const Event &e) override;

    /** Wire the shared modal text editor used for the (long) Meta Txt field. */
    void setTextEditor(TextInputView *e)
    {
        editor_ = e;
    }

    Screen &screen() override
    {
        return screen_;
    }

private:
    enum Row : uint8_t {
        RowCallsign,
        RowMeta,
        RowCan,
        RowCanRx,
        RowCount,
    };

    void writeValueText(uint8_t row);
    void beginEdit();
    void endEdit();
    void adjust(int dir);        //< value rows (CAN / CAN RX)
    void callsignCycle(int dir); //< cycle the char under the cursor
    void callsignMove(int dir);  //< move the cursor (extend at the end)
    void callsignConfirm();      //< strip + write to settings.callsign
    void announceCursorChar();   //< speak the char under the cursor (voice)

    Screen screen_;
    Flex root_;
    TopBar topBar_;
    List list_;

    TextInputView *editor_ = nullptr; //< shared modal, for Meta Txt

    ListItem items_[RowCount] = {};
    char bufs_[RowCount][24] = {};
    bool editing_ = false;
    uint8_t editRow_ = 0;

    /* Callsign edit state: the buffer is edited through the shared TextInput
     * widget (inline bracket rendering), which owns the cursor. */
    char callBuf_[10] = {};
    TextInput callsign_;

    /* Change-gate mirrors. */
    char lastCall_[10] = { '\1' }; //< force first render
    char lastMeta_[53] = { '\1' }; //< force first render
    uint8_t lastCan_ = 0xFFu;
    uint8_t lastCanRx_ = 0xFFu;
};

} // namespace ortxui

#endif /* ORTX_UI_M17VIEW_HPP */
