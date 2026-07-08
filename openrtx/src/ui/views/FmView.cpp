/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/FmView.hpp"
#include "core/Event.hpp"
#include "core/cps.h"
#include "interfaces/keyboard.h"
#include "hwconfig.h"

#include <cstdio>

namespace ortxui
{

namespace
{
/* Encode/decode enable label, indexed by (rxToneEn << 1) | txToneEn — the same
 * order the classic UI uses (tx tone = encode, rx tone = decode). */
const char *const kEnableLabels[4] = { "None", "Encode", "Decode", "Both" };
} // namespace

void FmView::build()
{
    const int16_t W = CONFIG_SCREEN_WIDTH;
    const int16_t H = CONFIG_SCREEN_HEIGHT;

    static const char *const kLabels[RowCount] = {
        "CTCSS Tone",
        "CTCSS En.",
    };

    root_.setArea({ 0, 0, (uint16_t)W, (uint16_t)H });
    root_.setAxis(Axis::Column);

    topBar_.init("FM");
    setTopBar(&topBar_);

    for (uint8_t i = 0; i < RowCount; i++) {
        items_[i].label = kLabels[i];
        items_[i].value = bufs_[i];
        writeValueText(i);
    }

    list_.setItems(items_, RowCount);
    list_.setContentPad(4);
    list_.setFlag(FLAG_FOCUSABLE, true);
    list_.setGrow(1);

    root_.addChild(&topBar_);
    root_.addChild(&list_);
    screen_.addChild(&root_);
    root_.onLayout();
    screen_.focusFirst();

    screen_.markAllDirty();

    lastTone_ = state.channel.fm.txTone;
    lastEnFlags_ = (uint8_t)((state.channel.fm.txToneEn << 1)
                             | state.channel.fm.rxToneEn);
}

void FmView::writeValueText(uint8_t row)
{
    const fmInfo_t &fm = state.channel.fm; /* live source of truth */
    char inner[10];

    if (row == RowTone) {
        const uint16_t t = ctcss_tone[fm.txTone];
        snprintf(inner, sizeof(inner), "%u.%u Hz", (unsigned)(t / 10),
                 (unsigned)(t % 10));
    } else {
        const uint8_t idx = (uint8_t)((fm.rxToneEn << 1) | fm.txToneEn);
        snprintf(inner, sizeof(inner), "%s", kEnableLabels[idx]);
    }

    if (editing_ && (row == editRow_)) {
        snprintf(bufs_[row], sizeof(bufs_[row]), "<%s>", inner);
        vpSay(inner);
    } else {
        snprintf(bufs_[row], sizeof(bufs_[row]), "%s", inner);
    }
}

void FmView::beginEdit()
{
    editRow_ = (uint8_t)list_.selected();
    editing_ = true;
    writeValueText(editRow_);
    list_.invalidate();
}

void FmView::endEdit()
{
    editing_ = false;
    writeValueText(editRow_);
    list_.invalidate();
}

void FmView::adjust(int dir)
{
    fmInfo_t &fm = state.channel.fm;

    if (editRow_ == RowTone) {
        const int n = CTCSS_FREQ_NUM;
        int t = (int)fm.txTone + dir;
        t = ((t % n) + n) % n;  /* wrap both directions */
        fm.txTone = (uint8_t)t;
        fm.rxTone = (uint8_t)t; /* keep RX in step with TX, as classic does */
        lastTone_ = (uint8_t)t;
    } else {
        /* Cycle None -> Encode -> Decode -> Both. Classic keys the cycle on
         * (tx << 1 | rx), distinct from the display order. */
        int flags = (fm.txToneEn << 1) | fm.rxToneEn;
        flags = ((flags + dir) & 3);
        fm.txToneEn = (uint8_t)((flags >> 1) & 1);
        fm.rxToneEn = (uint8_t)(flags & 1);
        lastEnFlags_ = (uint8_t)((fm.txToneEn << 1) | fm.rxToneEn);
    }

    requestSyncRtx(); /* re-tune the radio with the new tone/enable */
    writeValueText(editRow_);
    list_.invalidate();
}

void FmView::syncFromState(const state_t &s)
{
    View::syncFromState(s); /* top bar */

    const fmInfo_t &fm = s.channel.fm;
    const uint8_t tone = fm.txTone;
    const uint8_t enFlags = (uint8_t)((fm.txToneEn << 1) | fm.rxToneEn);
    bool changed = false;

    if (!(editing_ && editRow_ == RowTone) && (tone != lastTone_)) {
        writeValueText(RowTone);
        lastTone_ = tone;
        changed = true;
    }
    if (!(editing_ && editRow_ == RowEnable) && (enFlags != lastEnFlags_)) {
        writeValueText(RowEnable);
        lastEnFlags_ = enFlags;
        changed = true;
    }

    if (changed)
        list_.invalidate();
}

NavIntent FmView::onEvent(const Event &e)
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
        return NavIntent::none(); /* swallow input while editing */
    }

    if (e.kind == EvKind::Key) {
        if ((e.keys & KEY_ESC) != 0u)
            return NavIntent::pop();
        if ((e.keys & KEY_ENTER) != 0u) {
            beginEdit();
            return NavIntent::none();
        }
    }

    screen_.dispatch(e);
    return NavIntent::none();
}

} // namespace ortxui
