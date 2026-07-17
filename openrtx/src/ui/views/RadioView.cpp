/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/RadioView.hpp"
#include "core/Event.hpp"
#include "interfaces/keyboard.h"
#include "hwconfig.h"

#include <cstdio>
#include <cstring>

namespace ortxui
{

namespace
{

/* KEY_0..KEY_9 occupy bits 0..9, so a set digit bit's position is its value. */
constexpr uint32_t kDigitMask = 0x03FFu;

/* Format a frequency in Hz as a compact "N.NN kHz/MHz" (trailing zeros
 * stripped), e.g. 600000 -> "600 kHz", 6250 -> "6.25 kHz", 5000000 -> "5 MHz".
 * Sub-kHz values fall back to "N Hz". */
void formatFreq(uint32_t hz, char *buf, size_t size)
{
    uint32_t div;
    int width;
    char prefix;

    if (hz >= 1000000u) {
        div = 1000000u;
        width = 6;
        prefix = 'M';
    } else if (hz >= 1000u) {
        div = 1000u;
        width = 3;
        prefix = 'k';
    } else {
        snprintf(buf, size, "%u Hz", (unsigned)hz);
        return;
    }

    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%u.%0*u", (unsigned)(hz / div), width,
             (unsigned)(hz % div));

    /* Strip trailing zeros, then a bare trailing dot. */
    size_t len = strlen(tmp);
    while ((len > 0) && (tmp[len - 1] == '0'))
        tmp[--len] = '\0';
    if ((len > 0) && (tmp[len - 1] == '.'))
        tmp[--len] = '\0';

    snprintf(buf, size, "%s %cHz", tmp, prefix);
}

/* Lay a decimal value into a slot field left-aligned, so it reads back as that
 * integer via its filled prefix (filled slot count = number of digits). */
void setSlotsDecimal(TextInput &f, uint32_t val)
{
    char tmp[12];
    const int n = snprintf(tmp, sizeof(tmp), "%u", (unsigned)val);
    f.clearSlots();
    const uint16_t cap = f.slotCount();
    uint16_t i = 0;
    for (; (i < (uint16_t)n) && (i < cap); i++)
        f.setSlotDigit(i, tmp[i] - '0');
    f.setCursorSlot(i);
}

} // namespace

void RadioView::build()
{
    const int16_t W = CONFIG_SCREEN_WIDTH;
    const int16_t H = CONFIG_SCREEN_HEIGHT;

    static const char *const kLabels[RowCount] = {
        "Offset",
        "Direction",
        "Step",
    };

    root_.setArea({ 0, 0, (uint16_t)W, (uint16_t)H });
    root_.setAxis(Axis::Column);

    topBar_.init("Radio");
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

    lastRx_ = state.channel.rx_frequency;
    lastTx_ = state.channel.tx_frequency;
    lastStep_ = state.step_index;
}

void RadioView::writeValueText(uint8_t row)
{
    const channel_t &ch = state.channel; /* live source of truth */
    char inner[16];

    if (row == RowOffset) {
        if (editing_ && (editRow_ == RowOffset)) {
            const unsigned v = (unsigned)offsetKhz();
            snprintf(bufs_[row], sizeof(bufs_[row]), "<%u kHz>", v);
            char clean[16];
            snprintf(clean, sizeof(clean), "%u kHz", v);
            vpSay(clean);
            return;
        }
        const uint32_t off = (ch.tx_frequency >= ch.rx_frequency) ?
                                 (ch.tx_frequency - ch.rx_frequency) :
                                 (ch.rx_frequency - ch.tx_frequency);
        formatFreq(off, inner, sizeof(inner));
    } else if (row == RowDirection) {
        inner[0] = (ch.tx_frequency >= ch.rx_frequency) ? '+' : '-';
        inner[1] = '\0';
    } else {
        formatFreq(freq_steps[state.step_index], inner, sizeof(inner));
    }

    if (editing_ && (row == editRow_)) {
        snprintf(bufs_[row], sizeof(bufs_[row]), "<%s>", inner);
        vpSay(inner);
    } else {
        snprintf(bufs_[row], sizeof(bufs_[row]), "%s", inner);
    }
}

void RadioView::beginEdit()
{
    editRow_ = (uint8_t)list_.selected();
    editing_ = true;
    if (editRow_ == RowOffset) {
        /* Pre-fill the keypad with the current offset (magnitude + direction) so
         * re-selecting and pressing ENTER preserves the split; the previous code
         * reset it to 0, which wiped the offset on re-entry. */
        const channel_t &ch = state.channel;
        offsetNeg_ = (ch.tx_frequency < ch.rx_frequency);
        const uint32_t mag = (offsetNeg_ ? ch.rx_frequency - ch.tx_frequency :
                                           ch.tx_frequency - ch.rx_frequency)
                           / 1000u;
        offsetInput_.configureSlots(offsetBuf_, sizeof(offsetBuf_), "999999",
                                    '-', TextInput::CursorStyle::Bracket,
                                    FONT_SIZE_8PT);
        setSlotsDecimal(offsetInput_, mag);
        offsetPristine_ = true;
    }
    writeValueText(editRow_);
    list_.invalidate();
}

void RadioView::endEdit()
{
    editing_ = false;
    writeValueText(editRow_);
    list_.invalidate();
}

void RadioView::adjust(int dir)
{
    channel_t &ch = state.channel;

    if (editRow_ == RowDirection) {
        /* Mirror the TX split about RX: tx' = 2*rx - tx. A simplex channel
         * (tx == rx) is unaffected. */
        const int64_t rx = ch.rx_frequency;
        const int64_t tx = ch.tx_frequency;
        const int64_t mirrored = 2 * rx - tx;
        if (mirrored > 0) {
            ch.tx_frequency = (uint32_t)mirrored;
            lastTx_ = ch.tx_frequency;
            requestSyncRtx();
        }
    } else { /* RowStep: cycle the tuning step (no rtx effect) */
        const int n = (int)n_freq_steps;
        int s = ((int)state.step_index + dir) % n;
        if (s < 0)
            s += n;
        state.step_index = (uint8_t)s;
        lastStep_ = state.step_index;
    }

    writeValueText(editRow_);
    list_.invalidate();
}

uint32_t RadioView::offsetKhz() const
{
    /* The filled slots read left-to-right as a decimal kHz value. */
    uint32_t v = 0;
    const uint16_t n = offsetInput_.filledSlots();
    for (uint16_t o = 0; o < n; o++)
        v = v * 10u + (uint32_t)offsetInput_.slotDigit(o);
    return v;
}

void RadioView::offsetApply()
{
    /* Apply the entered magnitude in the current direction (keeping a '-' split
     * negative), so re-selecting + ENTER round-trips the exact offset. */
    const int64_t off = (int64_t)offsetKhz() * 1000;
    const int64_t tx = (int64_t)state.channel.rx_frequency
                     + (offsetNeg_ ? -off : off);
    if ((tx >= 0) && (tx <= 0xFFFFFFFF)) {
        state.channel.tx_frequency = (uint32_t)tx;
        lastTx_ = state.channel.tx_frequency;
        requestSyncRtx();
    }
    endEdit();
}

void RadioView::syncFromState(const state_t &s)
{
    View::syncFromState(s); /* top bar */

    const channel_t &ch = s.channel;
    bool changed = false;

    /* Offset + Direction both derive from the TX/RX pair. */
    if (!(editing_ && editRow_ == RowOffset)
        && ((ch.tx_frequency != lastTx_) || (ch.rx_frequency != lastRx_))) {
        writeValueText(RowOffset);
        changed = true;
    }
    if (!(editing_ && editRow_ == RowDirection)
        && ((ch.tx_frequency != lastTx_) || (ch.rx_frequency != lastRx_))) {
        writeValueText(RowDirection);
        changed = true;
    }
    if ((ch.tx_frequency != lastTx_) || (ch.rx_frequency != lastRx_)) {
        lastTx_ = ch.tx_frequency;
        lastRx_ = ch.rx_frequency;
    }

    if (!(editing_ && editRow_ == RowStep) && (s.step_index != lastStep_)) {
        writeValueText(RowStep);
        lastStep_ = s.step_index;
        changed = true;
    }

    if (changed)
        list_.invalidate();
}

NavIntent RadioView::onEvent(const Event &e)
{
    if (editing_) {
        /* Offset uses keypad entry; the other rows cycle with up/down. */
        if (editRow_ == RowOffset) {
            if (e.kind == EvKind::Key) {
                const uint32_t k = e.keys;
                if ((k & KEY_ENTER) != 0u) {
                    offsetApply();
                } else if ((k & KEY_ESC) != 0u) {
                    endEdit();
                } else {
                    /* Digits retype the value, '*' backspaces -- through the
                     * shared key contract, so delete is '*' here as everywhere
                     * (arrows no longer delete). The pre-filled value is pristine
                     * until the first digit clears it. */
                    const bool digit = ((k & kDigitMask) != 0u);
                    if (digit || ((k & KEY_STAR) != 0u)) {
                        if (offsetPristine_) {
                            if (digit)
                                offsetInput_.clearSlots();
                            offsetPristine_ = false;
                        }
                        offsetInput_.handleKey(k, 0);
                        writeValueText(RowOffset);
                        list_.invalidate();
                    }
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
            beginEdit();
            return NavIntent::none();
        }
    }

    screen_.dispatch(e);
    return NavIntent::none();
}

} // namespace ortxui
