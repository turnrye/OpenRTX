/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/M17View.hpp"
#include "views/TextInputView.hpp"
#include "core/Event.hpp"
#include "core/Layout.hpp"
#include "interfaces/keyboard.h"
#include "interfaces/delays.h"
#include "core/state.h"
#include "core/voicePrompts.h"
#include "core/voicePromptUtils.h"
#include "style/Charsets.hpp"
#include "hwconfig.h"

#include <cstdio>
#include <cstring>

namespace ortxui
{

void M17View::build()
{
    const int16_t W = CONFIG_SCREEN_WIDTH;
    const int16_t H = CONFIG_SCREEN_HEIGHT;
    const bool regular = (sizeClass() == SizeClass::Regular);

    static const char *const kLabels[RowCount] = {
        "Callsign",
        "Meta Txt",
        "CAN",
        "CAN RX",
    };

    root_.setArea({ 0, 0, (uint16_t)W, (uint16_t)H });
    root_.setAxis(Axis::Column);

    topBar_.init("M17");
    setTopBar(&topBar_);

    for (uint8_t i = 0; i < RowCount; i++) {
        items_[i].label = kLabels[i];
        items_[i].value = bufs_[i];
        writeValueText(i);
    }
    /* Meta Txt can be up to 52 chars — too long for the per-row scratch — so
     * point it straight at the live settings buffer and let the List ellipsize
     * it to fit the value column. */
    items_[RowMeta].value = state.settings.M17_meta_text;

    /* CAN RX (check the CAN on receive) is a boolean -> checkbox, not a value. */
    items_[RowCanRx].checkbox = true;
    items_[RowCanRx].checked = state.settings.m17_can_rx;
    items_[RowCanRx].value = nullptr;

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
}

void M17View::writeValueText(uint8_t row)
{
    const settings_t &st = state.settings; /* live source of truth */

    switch (row) {
        case RowCallsign: {
            if (editing_ && (editRow_ == RowCallsign)) {
                /* Render the buffer with the cursor character in brackets. */
                callsign_.formatBracketed(bufs_[row], sizeof(bufs_[row]));
            } else {
                snprintf(bufs_[row], sizeof(bufs_[row]), "%s", st.callsign);
            }
            return;
        }
        case RowMeta:
            /* The value points straight at the live settings buffer (set in
             * build); the List ellipsizes it. Nothing to format here. */
            return;
        case RowCan: {
            const bool ed = editing_ && (editRow_ == RowCan);
            snprintf(bufs_[row], sizeof(bufs_[row]), ed ? "<%u>" : "%u",
                     (unsigned)st.m17_can);
            if (ed) {
                char c[6];
                snprintf(c, sizeof(c), "%u", (unsigned)st.m17_can);
                vpSay(c);
            }
            return;
        }
        default: { /* RowCanRx */
            const bool ed = editing_ && (editRow_ == RowCanRx);
            const char *v = st.m17_can_rx ? "On" : "Off";
            snprintf(bufs_[row], sizeof(bufs_[row]), ed ? "<%s>" : "%s", v);
            if (ed)
                vpSay(v);
            return;
        }
    }
}

void M17View::beginEdit()
{
    editRow_ = (uint8_t)list_.selected();
    editing_ = true;

    if (editRow_ == RowCallsign) {
        strncpy(callBuf_, state.settings.callsign, sizeof(callBuf_) - 1);
        callBuf_[sizeof(callBuf_) - 1] = '\0';
        callsign_.configure(callBuf_, sizeof(callBuf_), CHARSET_CALLSIGN,
                            TextInput::Mode::SingleLine,
                            TextInput::CursorStyle::Bracket, FONT_SIZE_8PT);
        callsign_.setMultiTap(MTAP_CALLSIGN);
        callsign_.begin();
    }

    writeValueText(editRow_);
    list_.invalidate();
}

void M17View::endEdit()
{
    editing_ = false;
    writeValueText(editRow_);
    list_.invalidate();
}

void M17View::adjust(int dir)
{
    /* Only CAN is an adjustable value now (CAN RX is a checkbox). */
    settings_t &st = state.settings;
    st.m17_can = (uint8_t)((st.m17_can + dir + 16) % 16);
    lastCan_ = st.m17_can;

    writeValueText(RowCan);
    list_.invalidate();
}

void M17View::applyToggle(uint16_t row, bool on)
{
    if (row == RowCanRx) {
        state.settings.m17_can_rx = on;
        lastCanRx_ = on;
    }
}

void M17View::announceCursorChar()
{
    /* Speak the character under the cursor (phonetically at higher verbosity)
     * so a callsign can be edited by ear. */
    if (state.settings.vpLevel >= vpLow)
        vp_announceInputChar(callsign_.cursorChar());
}

void M17View::callsignCycle(int dir)
{
    callsign_.cycle(dir);
    writeValueText(RowCallsign);
    list_.invalidate();
    announceCursorChar();
}

void M17View::callsignMove(int dir)
{
    callsign_.moveCursor(dir);
    writeValueText(RowCallsign);
    list_.invalidate();
    announceCursorChar();
}

void M17View::callsignConfirm()
{
    /* Strip trailing spaces, then commit. */
    callsign_.stripTrailingSpaces();

    strncpy(state.settings.callsign, callBuf_,
            sizeof(state.settings.callsign) - 1);
    state.settings.callsign[sizeof(state.settings.callsign) - 1] = '\0';
    memcpy(lastCall_, state.settings.callsign, sizeof(lastCall_));

    endEdit();
}

void M17View::syncFromState(const state_t &s)
{
    View::syncFromState(s); /* top bar */

    const settings_t &st = s.settings;
    bool changed = false;

    if (!(editing_ && editRow_ == RowCallsign)
        && (strncmp(st.callsign, lastCall_, sizeof(lastCall_)) != 0)) {
        memcpy(lastCall_, st.callsign, sizeof(lastCall_));
        writeValueText(RowCallsign);
        changed = true;
    }
    /* Meta Txt is edited by the modal editor; pick up its result on return. */
    if (strncmp(st.M17_meta_text, lastMeta_, sizeof(lastMeta_)) != 0) {
        memcpy(lastMeta_, st.M17_meta_text, sizeof(lastMeta_));
        writeValueText(RowMeta);
        changed = true;
    }
    if (!(editing_ && editRow_ == RowCan) && (st.m17_can != lastCan_)) {
        lastCan_ = st.m17_can;
        writeValueText(RowCan);
        changed = true;
    }
    if (st.m17_can_rx != lastCanRx_) {
        lastCanRx_ = st.m17_can_rx;
        items_[RowCanRx].checked = st.m17_can_rx;
        changed = true;
    }

    if (changed)
        list_.invalidate();
}

NavIntent M17View::onEvent(const Event &e)
{
    if (editing_) {
        if (editRow_ == RowCallsign) {
            if (e.kind == EvKind::Encoder) {
                /* Knob moves the cursor on arrow-less radios, cycles elsewhere. */
                if (kbdHasArrows())
                    callsignCycle(e.encoder);
                else
                    callsignMove(e.encoder > 0 ? +1 : -1);
            } else if (e.kind == EvKind::Key) {
                const uint32_t k = e.keys;
                if ((k & KEY_ENTER) != 0u) {
                    callsignConfirm();
                } else if ((k & KEY_ESC) != 0u) {
                    endEdit();
                } else if ((k & KBD_CHAR_MASK) != 0u) {
                    /* Numeric-keypad multi-tap; '*' backspaces. */
                    const uint8_t ki =
                        static_cast<uint8_t>(__builtin_ctz(k & KBD_CHAR_MASK));
                    if (ki == 10u)
                        callsign_.backspace();
                    else
                        callsign_.tapKey(ki, getTick());
                    writeValueText(RowCallsign);
                    list_.invalidate();
                    announceCursorChar();
                } else if ((k & KEY_UP) != 0u) {
                    callsignCycle(+1);
                } else if ((k & KEY_DOWN) != 0u) {
                    callsignCycle(-1);
                } else if ((k & KEY_LEFT) != 0u) {
                    callsignMove(-1);
                } else if ((k & KEY_RIGHT) != 0u) {
                    callsignMove(+1);
                }
            }
            return NavIntent::none();
        }

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
            /* CAN RX is a checkbox: ENTER toggles it in place. */
            ListItem *it = list_.selectedItem();
            if ((it != nullptr) && it->checkbox) {
                it->checked = !it->checked;
                applyToggle(list_.selected(), it->checked);
                list_.invalidate();
                vpSay(it->checked ? "On" : "Off"); /* toggle: no nav change */
                return NavIntent::none();
            }
            /* Meta Txt is long free text: hand it to the modal editor. The
             * others edit in place with the cursor cycle. */
            if (list_.selected() == RowMeta) {
                if (editor_ != nullptr) {
                    editor_->open("Meta Text", state.settings.M17_meta_text,
                                  sizeof(state.settings.M17_meta_text),
                                  CHARSET_TEXT, MTAP_TEXT, /*multiline=*/true,
                                  /*rtx=*/false);
                    return NavIntent::push(editor_);
                }
                return NavIntent::none();
            }
            beginEdit();
            return NavIntent::none();
        }
    }

    screen_.dispatch(e);
    return NavIntent::none();
}

} // namespace ortxui
