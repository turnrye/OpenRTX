/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/M17View.hpp"
#include "core/Event.hpp"
#include "interfaces/keyboard.h"
#include "hwconfig.h"

#include <cstdio>
#include <cstring>

namespace ortxui
{

namespace
{
/* Callsign edit alphabet (space first so a fresh slot reads as blank). */
const char kCharset[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/.";
constexpr int kCharsetLen = (int)(sizeof(kCharset) - 1);
constexpr uint8_t kCallMax = 9; //< settings.callsign is char[10]

int charsetIndex(char c)
{
    for (int i = 0; i < kCharsetLen; i++)
        if (kCharset[i] == c)
            return i;
    return 0;
}
} // namespace

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
                char *o = bufs_[row];
                size_t rem = sizeof(bufs_[row]);
                for (uint8_t i = 0; (i < callLen_) && (rem > 4); i++) {
                    int n = (i == cursor_) ?
                                snprintf(o, rem, "[%c]", callBuf_[i]) :
                                snprintf(o, rem, "%c", callBuf_[i]);
                    o += n;
                    rem -= n;
                }
                *o = '\0';
            } else {
                snprintf(bufs_[row], sizeof(bufs_[row]), "%s", st.callsign);
            }
            return;
        }
        case RowMeta: {
            /* Read-only; classic truncates to 7 chars + '*' when longer.
             * memcpy (not snprintf %s) so the compiler sees the bounded
             * length and doesn't warn about the 53-byte source. */
            const size_t ml = strnlen(st.M17_meta_text,
                                      sizeof(st.M17_meta_text));
            if (ml > 7) {
                memcpy(bufs_[row], st.M17_meta_text, 7);
                bufs_[row][7] = '*';
                bufs_[row][8] = '\0';
            } else {
                memcpy(bufs_[row], st.M17_meta_text, ml);
                bufs_[row][ml] = '\0';
            }
            return;
        }
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
        callLen_ = (uint8_t)strlen(callBuf_);
        if (callLen_ == 0) { /* start from a single editable blank */
            callBuf_[0] = ' ';
            callBuf_[1] = '\0';
            callLen_ = 1;
        }
        cursor_ = 0;
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
    settings_t &st = state.settings;

    if (editRow_ == RowCan) {
        st.m17_can = (uint8_t)((st.m17_can + dir + 16) % 16);
        lastCan_ = st.m17_can;
    } else { /* RowCanRx: any step toggles */
        st.m17_can_rx = !st.m17_can_rx;
        lastCanRx_ = st.m17_can_rx;
    }

    writeValueText(editRow_);
    list_.invalidate();
}

void M17View::callsignCycle(int dir)
{
    int idx = charsetIndex(callBuf_[cursor_]);
    idx = ((idx + dir) % kCharsetLen + kCharsetLen) % kCharsetLen;
    callBuf_[cursor_] = kCharset[idx];
    writeValueText(RowCallsign);
    list_.invalidate();
}

void M17View::callsignMove(int dir)
{
    if (dir < 0) {
        if (cursor_ > 0)
            cursor_--;
    } else {
        if (cursor_ + 1 < callLen_) {
            cursor_++;
        } else if (callLen_ < kCallMax) {
            /* Extend with a blank and step onto it. */
            callBuf_[callLen_] = ' ';
            callBuf_[callLen_ + 1] = '\0';
            callLen_++;
            cursor_ = (uint8_t)(callLen_ - 1);
        }
    }
    writeValueText(RowCallsign);
    list_.invalidate();
}

void M17View::callsignConfirm()
{
    /* Strip trailing spaces, then commit. */
    while ((callLen_ > 0) && (callBuf_[callLen_ - 1] == ' '))
        callBuf_[--callLen_] = '\0';

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
    if (!(editing_ && editRow_ == RowCan) && (st.m17_can != lastCan_)) {
        lastCan_ = st.m17_can;
        writeValueText(RowCan);
        changed = true;
    }
    if (!(editing_ && editRow_ == RowCanRx) && (st.m17_can_rx != lastCanRx_)) {
        lastCanRx_ = st.m17_can_rx;
        writeValueText(RowCanRx);
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
                callsignCycle(e.encoder);
            } else if (e.kind == EvKind::Key) {
                if ((e.keys & KEY_ENTER) != 0u)
                    callsignConfirm();
                else if ((e.keys & KEY_ESC) != 0u)
                    endEdit();
                else if ((e.keys & KEY_UP) != 0u)
                    callsignCycle(+1);
                else if ((e.keys & KEY_DOWN) != 0u)
                    callsignCycle(-1);
                else if ((e.keys & KEY_LEFT) != 0u)
                    callsignMove(-1);
                else if ((e.keys & KEY_RIGHT) != 0u)
                    callsignMove(+1);
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
            /* Meta Txt is read-only for now; the rest open an editor. */
            if (list_.selected() != RowMeta)
                beginEdit();
            return NavIntent::none();
        }
    }

    screen_.dispatch(e);
    return NavIntent::none();
}

} // namespace ortxui
