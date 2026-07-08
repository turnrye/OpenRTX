/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/VfoView.hpp"
#include "style/Symbols.hpp"
#include "style/Charsets.hpp"
#include "core/Event.hpp"
#include "core/Layout.hpp"
#include "interfaces/delays.h"
#include "core/utils.h"
#include "core/cps.h"
#include "rtx/rtx.h"
#include "interfaces/keyboard.h"
#include "interfaces/platform.h"
#include "interfaces/cps_io.h"
#include "core/voicePrompts.h"
#include "core/voicePromptUtils.h"
#include "hwconfig.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace ortxui
{

namespace
{

/* Digits per RX/TX frequency in keypad entry (classic FREQ_DIGITS). */
constexpr uint8_t kFreqDigits = 7;

/* The TX power meter is a dB (log) scale relative to the radio's maximum
 * output. There is no per-device max-power field yet (hwInfo_t only carries
 * bands), so assume 5 W — the rating of essentially every OpenRTX-supported
 * radio; when a hwInfo max-power field lands, read it here instead. The meter
 * empties kPowerSpanDb below the max, so full power fills every dot and each
 * halving of power (~3 dB) drops a few dots. */
constexpr uint32_t kMaxPowerMw = 5000;
constexpr float kPowerSpanDb = 10.0f;

/* Number of filled dots (0..total) for a TX power on the dB scale. Any nonzero
 * power lights at least one dot (you are transmitting something). */
uint8_t powerToDots(uint32_t mw, uint8_t total)
{
    if (mw == 0u)
        return 0;
    const float db =
        10.0f
        * std::log10(static_cast<float>(mw) / static_cast<float>(kMaxPowerMw));
    float f = total * (1.0f + db / kPowerSpanDb);
    if (f < 1.0f)
        f = 1.0f;
    if (f > total)
        f = static_cast<float>(total);
    return static_cast<uint8_t>(std::lround(f));
}

/* Return the 0-based digit for a bare number key (KEY_0..KEY_9 are bits 0..9),
 * or -1 if the mask holds no digit key. */
int digitFromKeys(uint32_t keys)
{
    const uint32_t digits = keys & 0x3FFu;
    if (digits == 0u)
        return -1;
    return __builtin_ctz(digits);
}

/* Add one decimal digit at 1-based position `pos` to a frequency being entered,
 * most-significant digit first (ported from the classic _ui_freq_add_digit). */
freq_t freqAddDigit(freq_t freq, uint8_t pos, uint8_t number)
{
    freq_t coefficient = 100;
    for (uint8_t i = 0; i < kFreqDigits - pos; i++)
        coefficient *= 10;
    return freq + (freq_t)number * coefficient;
}

/* True if a frequency falls inside one of the radio's supported bands
 * (ported from the classic _ui_freq_check_limits). */
bool freqInBand(freq_t freq)
{
    const hwInfo_t *hw = platform_getHwInfo();
    if (hw->vhf_band && (freq >= (freq_t)hw->vhf_minFreq * 1000000u)
        && (freq <= (freq_t)hw->vhf_maxFreq * 1000000u))
        return true;
    if (hw->uhf_band && (freq >= (freq_t)hw->uhf_minFreq * 1000000u)
        && (freq <= (freq_t)hw->uhf_maxFreq * 1000000u))
        return true;
    return false;
}

} // namespace

void VfoView::build()
{
    const int16_t W = CONFIG_SCREEN_WIDTH;
    const int16_t H = CONFIG_SCREEN_HEIGHT;
    const bool regular = (sizeClass() == SizeClass::Regular);

    root_.setArea({ 0, 0, (uint16_t)W, (uint16_t)H });
    root_.setAxis(Axis::Column);

    /* Shared top bar with a blank title (VFO shows only the status cluster). */
    topBar_.init("");
    setTopBar(&topBar_);

    /* Frequency hero. On the roomy screen it is a little taller than the text
     * needs so the mode stack breathes; on the tiny mono screen it is trimmed
     * to just clear the frequency and the mode/tone stack, freeing vertical
     * room below so the channel line and meter both fit within 64px. */
    hero_.setBasis(regular ? 40 : 26);

    /* Channel row, all one text size: the "where am I" slot (index / "VFO") on
     * the left, the channel name or M17 destination in the middle, and a
     * right-aligned secondary detail (M17 CAN / FM tone) sitting directly under
     * the mode label in the hero. The compact screen uses a smaller font so a
     * full line (ascent + descent) still fits its tight budget. */
    const fontSize_t chanFont = regular ? FONT_SIZE_8PT : FONT_SIZE_6PT;
    chanRow_.setAxis(Axis::Row);
    chanRow_.setAlign(Align::Center);
    chanRow_.setJustify(Justify::Start);
    chanRow_.setPadding(4, 0);
    chanRow_.setGap(6);
    /* Make the row as tall as the font's full line height (ascent + descent) so
     * descenders (and the '@' / caps of an M17 destination) are not clipped
     * against the meter row below. */
    chanRow_.setBasis(gfx_getFontLineHeight(chanFont));

    chanIdx_.setFont(chanFont);
    chanIdx_.setAlign(TEXT_ALIGN_LEFT);
    chanIdx_.setColor(Sem::OnSurfaceMuted);
    /* Wide enough for the widest content ("VFO" ~30px @ 8pt, ~23px @ 6pt) so
     * the box clip never cuts a glyph. */
    chanIdx_.setBasis(regular ? 34 : 26);

    chanName_.setFont(chanFont);
    chanName_.setAlign(TEXT_ALIGN_LEFT);
    chanName_.setColor(Sem::Primary); /* blue channel name */
    chanName_.setGrow(1);
    /* A long name/destination (e.g. "@HELLOWORLD") is truncated with an
     * ellipsis to its Flex-assigned width rather than wrapping into the
     * meter row below. */
    chanName_.setOverflow(Label::Overflow::Ellipsize);

    /* Detail = a small "CAN" tag + the value at the row font (or just a tone
     * value in FM), right-aligned and sharing one baseline. Wide enough for
     * "CAN" + a value without eating the name. */
    chanDetail_.setRun(0, FONT_SIZE_5PT, Sem::OnSurfaceMuted,
                       TextRow::Side::Right); /* tag */
    chanDetail_.setRun(1, chanFont, Sem::OnSurfaceMuted,
                       TextRow::Side::Right); /* value */
    chanDetail_.setGap(3);
    chanDetail_.setBasis(regular ? 48 : 34);

    chanRow_.addChild(&chanIdx_);
    chanRow_.addChild(&chanName_);
    chanRow_.addChild(&chanDetail_);

    /* Signal meter. */
    meter_.setBasis(regular ? 14 : 12);
    meter_.setColor(Sem::RxSuccess);
    meter_.setLabel("RX");

    /* Open space below pushes the readout to the top. */
    spacer_.setGrow(1);

    root_.addChild(&topBar_);
    root_.addChild(&hero_);
    root_.addChild(&chanRow_);
    root_.addChild(&meter_);
    root_.addChild(&spacer_);
    screen_.addChild(&root_);
    root_.onLayout();

    screen_.markAllDirty();
}

void VfoView::syncMode(const channel_t &ch)
{
    const char *m1 = "--";
    switch (ch.mode) {
        case OPMODE_FM:
            m1 = (ch.bandwidth == BW_25) ? "WFM" : "NFM";
            break;
        case OPMODE_DMR:
            m1 = "DMR";
            break;
        case OPMODE_M17:
            m1 = SYMBOL_M17; /* the M17 wordmark logo glyph */
            break;
        default:
            break;
    }

    /* Channel-row detail (right, under the mode): a small "CAN" tag + number in
     * M17 -- but only when the CAN is actually gating traffic: on TX (you key
     * up on it) or when RX is filtered to it (m17_can_rx). While RX is
     * promiscuous and idle the CAN filters nothing, so hide it. In FM it is the
     * CTCSS/DCS tone (no tag) matching the current direction (the TX/encode tone
     * while transmitting, else the RX/decode tone), shown only when that
     * direction's tone is enabled. Blank otherwise. */
    const char *tag = "";
    modeSub_[0] = '\0';
    if (ch.mode == OPMODE_M17) {
        const bool txing = (rtx_getStatus()->opStatus == TX);
        if (txing || state.settings.m17_can_rx) {
            tag = "CAN";
            snprintf(modeSub_, sizeof(modeSub_), "%u",
                     (unsigned)state.settings.m17_can);
        }
    } else if (ch.mode == OPMODE_FM) {
        const bool txing = (rtx_getStatus()->opStatus == TX);
        const bool showTx = txing && (ch.fm.txToneEn != 0u);
        const bool showRx = !txing && (ch.fm.rxToneEn != 0u);
        if (showTx || showRx) {
            const uint8_t idx = showTx ? ch.fm.txTone : ch.fm.rxTone;
            const uint16_t t = ctcss_tone[idx];
            snprintf(modeSub_, sizeof(modeSub_), "%u.%u", (unsigned)(t / 10),
                     (unsigned)(t % 10));
        }
    }

    /* Repaint only when the visible pair changes: covers mode/bandwidth, the
     * FM tone, and the dynamic M17 CAN (settings + TX/RX). The mode label sits
     * on the frequency baseline (hero); the detail sits under it on the channel
     * row. */
    char sig[28];
    snprintf(sig, sizeof(sig), "%s|%s|%s", m1, tag, modeSub_);
    if (strcmp(sig, modeCache_) == 0)
        return;
    strncpy(modeCache_, sig, sizeof(modeCache_) - 1);
    modeCache_[sizeof(modeCache_) - 1] = '\0';

    hero_.setMode(m1);
    hero_.invalidate();
    chanDetail_.setText(0, tag);
    chanDetail_.setText(1, modeSub_);
    chanDetail_.invalidate();
}

void VfoView::syncMeter(const state_t &s)
{
    /* Live TX/RX comes from the rtx module (state.rtxStatus is only the
     * requested mode); this matches how the classic UI reads it. */
    const uint8_t status = rtx_getStatus()->opStatus;
    const bool changed = (status != lastStatus_) || (s.rssi != lastRssi_);
    if (!changed)
        return;

    if (status == TX) {
        const uint32_t mw = s.channel.power;

        meter_.setLabel("TX");
        meter_.setColor(Sem::TxDanger);
        meter_.setShowScale(false);
        /* Fill dots on a dB power scale relative to the radio's max output. */
        meter_.setLevel(powerToDots(mw, 9), 9);

        if (mw >= 1000u)
            snprintf(readoutBuf_, sizeof(readoutBuf_), "%lu.%luW",
                     (unsigned long)(mw / 1000u),
                     (unsigned long)((mw % 1000u) / 100u));
        else
            snprintf(readoutBuf_, sizeof(readoutBuf_), "%lumW",
                     (unsigned long)mw);
    } else {
        const uint8_t level = rssiToSlevel(s.rssi);
        const uint8_t filled = (level > 9u) ? 9u : level;

        meter_.setLabel("RX");
        meter_.setColor(Sem::RxSuccess);
        meter_.setShowScale(true);
        meter_.setLevel(filled, 9);

        if (level > 9u)
            snprintf(readoutBuf_, sizeof(readoutBuf_), "+%u",
                     (unsigned)((level - 9u) * 10u));
        else
            snprintf(readoutBuf_, sizeof(readoutBuf_), "S%u", (unsigned)level);
    }

    meter_.setReadout(readoutBuf_);
    meter_.invalidate();

    lastStatus_ = status;
    lastRssi_ = s.rssi;
}

void VfoView::syncFromState(const state_t &s)
{
    View::syncFromState(s); /* shared top bar */

    /* While entering a frequency, the hero and channel line show the keypad
     * buffer (driven by refreshInput()); don't let live state overwrite them. */
    if (!inputActive_) {
        const channel_t &ch = s.channel;

        /* While actually transmitting a channel with an RX/TX split (repeater
         * offset), show the TX frequency the radio is keyed up on; otherwise
         * (idle, or simplex) show the RX frequency. Gating on the *shown* value
         * repaints on both frequency changes and TX start/stop. */
        const bool txing = (rtx_getStatus()->opStatus == TX);
        const freq_t shown = (txing && (ch.tx_frequency != ch.rx_frequency)) ?
                                 ch.tx_frequency :
                                 ch.rx_frequency;
        if ((uint32_t)shown != lastFreq_) {
            hero_.setFreq((uint32_t)shown);
            hero_.invalidate();
            lastFreq_ = (uint32_t)shown;
        }

        syncMode(ch);

        /* Channel line (skipped while editing the destination, which drives it
         * directly): VFO mode shows "VFO" (or the M17 destination) with no
         * index; memory mode shows the 1-based index + channel name. Compose
         * the slot text and repaint only on a string change. */
        if (!dstEditing_) {
            char idxText[8];
            char nameText[40];
            composeChanLine(s, idxText, nameText, sizeof(nameText));

            if (strncmp(idxText, idxCache_, sizeof(idxCache_)) != 0) {
                strncpy(idxCache_, idxText, sizeof(idxCache_) - 1);
                idxCache_[sizeof(idxCache_) - 1] = '\0';
                chanIdx_.setText(idxCache_);
                chanIdx_.invalidate();
            }
            if (strncmp(nameText, nameCache_, sizeof(nameCache_)) != 0) {
                strncpy(nameCache_, nameText, sizeof(nameCache_) - 1);
                nameCache_[sizeof(nameCache_) - 1] = '\0';
                chanName_.setText(nameCache_);
                chanName_.invalidate();
            }
        }
    }

    syncMeter(s);
}

NavIntent VfoView::onEvent(const Event &e)
{
    if (dstEditing_)
        return onDstEditEvent(e);

    if (inputActive_)
        return onInputEvent(e);

    /* The knob tunes in VFO mode and steps channels in memory mode. */
    if (e.kind == EvKind::Encoder) {
        clearFmTone(); /* any input other than # drops the momentary tone */
        if (state.tuner_mode == VFO)
            stepFreq(e.encoder);
        else
            stepChannel(e.encoder);
        return NavIntent::none();
    }

    if (e.kind == EvKind::Key) {
        const uint32_t k = e.keys;

        /* The momentary FM tone latches on # and is cleared by the next other
         * key (classic parity). */
        if ((k & KEY_HASH) == 0u)
            clearFmTone();

        if ((k & KEY_ENTER) != 0u) {
            if (menu_ != nullptr)
                return NavIntent::push(menu_);
        } else if ((k & KEY_ESC) != 0u) {
            toggleVfoMem();
        } else if ((k & KEY_HASH) != 0u) {
            /* # opens the M17 destination editor, or keys the FM tone burst. */
            if (state.channel.mode == OPMODE_M17) {
                beginDstEdit();
            } else if ((state.channel.mode == OPMODE_FM)
                       && !state.tone_enabled) {
                state.tone_enabled = true;
                requestSyncRtx();
            }
        } else if ((k & KEY_UP) != 0u) {
            if (state.tuner_mode == VFO)
                stepFreq(+1);
            else
                stepChannel(+1);
        } else if ((k & KEY_DOWN) != 0u) {
            if (state.tuner_mode == VFO)
                stepFreq(-1);
            else
                stepChannel(-1);
        } else if (state.tuner_mode == VFO) {
            /* A digit opens the frequency keypad (VFO mode only). */
            const int d = digitFromKeys(k);
            if (d >= 0)
                beginInput((uint8_t)d);
        }
        return NavIntent::none();
    }

    screen_.dispatch(e);
    return NavIntent::none();
}

NavIntent VfoView::onInputEvent(const Event &e)
{
    if (e.kind != EvKind::Key)
        return NavIntent::none();

    const uint32_t k = e.keys;
    if ((k & KEY_ENTER) != 0u) {
        confirmInput();
    } else if ((k & KEY_ESC) != 0u) {
        exitInput(); /* discard the entry */
    } else if ((k & (KEY_UP | KEY_DOWN)) != 0u) {
        /* Toggle which frequency (RX/TX) is being entered. */
        inputTxSet_ = !inputTxSet_;
        inputPos_ = 0;
        refreshInput();
        if (state.settings.vpLevel >= vpLow)
            vp_announceInputReceiveOrTransmit(inputTxSet_, vpqDefault);
    } else {
        const int d = digitFromKeys(k);
        if (d >= 0)
            inputDigit((uint8_t)d);
    }
    return NavIntent::none();
}

void VfoView::stepFreq(int dir)
{
    const freq_t step = freq_steps[state.step_index];
    freq_t rx = state.channel.rx_frequency;
    freq_t tx = state.channel.tx_frequency;
    const freq_t nrx = (dir > 0) ? (rx + step) : (rx - step);
    const freq_t ntx = (dir > 0) ? (tx + step) : (tx - step);

    /* Reject a step that would push either edge out of band (underflow wraps
     * to a huge value and is rejected the same way). */
    if (!freqInBand(nrx) || !freqInBand(ntx))
        return;

    state.channel.rx_frequency = nrx;
    state.channel.tx_frequency = ntx;
    requestSyncRtx();
    announceFreq();
}

void VfoView::stepChannel(int dir)
{
    if (loadChannel((int16_t)((int)state.channel_index + dir))) {
        requestSyncRtx();
        if (state.settings.vpLevel >= vpLow)
            vp_announceChannelName(&state.channel,
                                   (uint16_t)(state.channel_index + 1),
                                   vp_getVoiceLevelQueueFlags());
    }
}

void VfoView::toggleVfoMem()
{
    if (state.tuner_mode == VFO) {
        /* VFO -> memory: remember the VFO, then load the current channel. */
        state.vfo_channel = state.channel;
        if (loadChannel((int16_t)state.channel_index)) {
            state.tuner_mode = CH;
            requestSyncRtx();
            if (state.settings.vpLevel >= vpLow)
                vp_announceChannelName(&state.channel,
                                       (uint16_t)(state.channel_index + 1),
                                       vp_getVoiceLevelQueueFlags());
        }
        /* An empty/invalid codeplug leaves us in VFO mode (no channel). */
    } else {
        /* Memory -> VFO: restore the saved VFO channel. */
        state.channel = state.vfo_channel;
        state.tuner_mode = VFO;
        requestSyncRtx();
        announceFreq();
    }
}

bool VfoView::loadChannel(int16_t index)
{
    const int16_t selected = index;
    int16_t readIndex = index;

    if (state.bank_enabled) {
        bankHdr_t bank = {};
        cps_readBankHeader(&bank, state.bank);
        if ((index < 0) || (index >= (int16_t)bank.ch_count))
            return false;
        readIndex = (int16_t)cps_readBankData(state.bank, (uint16_t)index);
    } else if (index < 0) {
        return false;
    }

    channel_t ch;
    if (cps_readChannel(&ch, (uint16_t)readIndex) != 0)
        return false;

    state.channel = ch;
    state.channel_index = (uint16_t)selected;
    return true;
}

void VfoView::beginInput(uint8_t firstDigit)
{
    inputActive_ = true;
    inputTxSet_ = false;
    inputPos_ = 0;
    newRx_ = 0;
    newTx_ = 0;
    inputDigit(firstDigit); /* also draws the initial entry chrome */
}

void VfoView::inputDigit(uint8_t digit)
{
    inputPos_++;

    if (!inputTxSet_) {
        if (inputPos_ == 1)
            newRx_ = 0;
        newRx_ = freqAddDigit(newRx_, inputPos_, digit);
        if (inputPos_ >= kFreqDigits) {
            /* RX complete: move on to the TX frequency. */
            inputTxSet_ = true;
            inputPos_ = 0;
            newTx_ = 0;
        }
    } else {
        if (inputPos_ == 1)
            newTx_ = 0;
        newTx_ = freqAddDigit(newTx_, inputPos_, digit);
        if (inputPos_ >= kFreqDigits) {
            applyInput();
            return;
        }
    }

    if (state.settings.vpLevel >= vpLow) {
        vp_flush();
        vp_queueInteger(digit);
        vp_play();
    }
    refreshInput();
}

void VfoView::confirmInput()
{
    if (!inputTxSet_) {
        /* Confirm RX, advance to TX entry. */
        inputTxSet_ = true;
        inputPos_ = 0;
        refreshInput();
        if (state.settings.vpLevel >= vpLow)
            vp_announceInputReceiveOrTransmit(true, vpqDefault);
    } else {
        /* If TX was left untouched, mirror RX onto it. */
        if (newTx_ == 0)
            newTx_ = newRx_;
        applyInput();
    }
}

void VfoView::applyInput()
{
    if (freqInBand(newRx_) && freqInBand(newTx_)) {
        state.channel.rx_frequency = newRx_;
        state.channel.tx_frequency = newTx_;
        requestSyncRtx();
        if (state.settings.vpLevel >= vpLow) {
            vp_flush();
            vp_announceFrequencies(newRx_, newTx_, vpqDefault);
            vp_play();
        }
    }
    exitInput();
}

void VfoView::exitInput()
{
    inputActive_ = false;

    /* Force the change-gated readout to repaint from live state next sync. */
    lastFreq_ = 0xFFFFFFFFu;
    modeCache_[0] = '\1';
    idxCache_[0] = '\1';
    nameCache_[0] = '\1';
    screen_.markAllDirty();
}

void VfoView::refreshInput()
{
    const uint32_t val = inputTxSet_ ? newTx_ : newRx_;
    hero_.setFreq(val);
    hero_.setMode(inputTxSet_ ? "TX" : "RX");
    hero_.invalidate();

    chanIdx_.setText("");
    chanName_.setText(inputTxSet_ ? "ENTER TX" : "ENTER RX");
    chanDetail_.setText(0, "");
    chanDetail_.setText(1, "");
    chanIdx_.invalidate();
    chanName_.invalidate();
    chanDetail_.invalidate();
}

void VfoView::clearFmTone()
{
    if (state.tone_enabled) {
        state.tone_enabled = false;
        requestSyncRtx();
    }
}

void VfoView::m17DstLabel(char *out, uint16_t sz)
{
    const char *dst = state.settings.m17_dest;
    if (dst[0] == '\0')
        snprintf(out, sz, "@ALL");
    else
        snprintf(out, sz, "@%s", dst);
}

void VfoView::composeChanLine(const state_t &s, char *idxOut, char *nameOut,
                              uint16_t nameSz)
{
    const channel_t &ch = s.channel;

    if (s.tuner_mode == VFO) {
        /* Far-left is the "where am I" slot: "VFO" here, the memory index in
         * memory mode. In M17 the destination fills the name slot; FM has no
         * name (just the frequency). The CAN lives in the mode stack, not here. */
        snprintf(idxOut, 8, "VFO");
        if (ch.mode == OPMODE_M17)
            m17DstLabel(nameOut, nameSz);
        else
            nameOut[0] = '\0';
    } else {
        snprintf(idxOut, 8, "%03u", s.channel_index + 1);
        snprintf(nameOut, nameSz, "%s", (ch.name[0] != '\0') ? ch.name : "---");
    }
}

void VfoView::renderDstEdit()
{
    /* Compose "@W1[A]W" with the cursor character in brackets, straight into
     * the channel-line name slot (no custom cursor rendering needed). The '@'
     * marks it as an M17 destination address and stays pinned while the
     * callsign body scrolls within the remaining width -- a leading/trailing
     * ellipsis marks where it is truncated, so the cursor is always visible. */
    const bool regular = (sizeClass() == SizeClass::Regular);
    const fontSize_t font = regular ? FONT_SIZE_10PT : FONT_SIZE_6PT;
    const uint16_t atW = gfx_getTextWidth(font, "@");
    const uint16_t boxW = chanName_.area().w;
    const uint16_t bodyW = (boxW > atW) ? static_cast<uint16_t>(boxW - atW) : 0;

    char body[36];
    dst_.formatWindow(body, sizeof(body), bodyW, font);
    snprintf(nameCache_, sizeof(nameCache_), "@%s", body);
    idxCache_[0] = '\0';
    chanIdx_.setText(idxCache_);
    chanName_.setText(nameCache_);
    chanIdx_.invalidate();
    chanName_.invalidate();
}

void VfoView::announceDstChar()
{
    if (state.settings.vpLevel >= vpLow)
        vp_announceInputChar(dst_.cursorChar());
}

void VfoView::beginDstEdit()
{
    strncpy(dstBuf_, state.settings.m17_dest, sizeof(dstBuf_) - 1);
    dstBuf_[sizeof(dstBuf_) - 1] = '\0';
    dst_.configure(dstBuf_, sizeof(dstBuf_), CHARSET_CALLSIGN,
                   TextInput::Mode::SingleLine, TextInput::CursorStyle::Bracket,
                   FONT_SIZE_8PT);
    dst_.setMultiTap(MTAP_CALLSIGN);
    dst_.begin();
    dstEditing_ = true;
    /* The channel-name font stays at its display size: renderDstEdit() scrolls
     * a cursor-visible window of the "@W1[A]W" string within the box instead of
     * shrinking the whole field to make a full-length destination fit. */
    renderDstEdit();
    screen_.markAllDirty();
    announceDstChar();
}

void VfoView::dstCycle(int dir)
{
    dst_.cycle(dir);
    renderDstEdit();
    announceDstChar();
}

void VfoView::dstMove(int dir)
{
    dst_.moveCursor(dir);
    renderDstEdit();
    announceDstChar();
}

void VfoView::endDstEdit(bool commit)
{
    if (commit) {
        /* Strip trailing spaces, then store the destination. */
        dst_.stripTrailingSpaces();
        strncpy(state.settings.m17_dest, dstBuf_,
                sizeof(state.settings.m17_dest) - 1);
        state.settings.m17_dest[sizeof(state.settings.m17_dest) - 1] = '\0';
        requestSyncRtx(); /* the destination feeds the M17 LSF */
    }

    dstEditing_ = false;
    /* Force the channel line to recompose from live state next sync. */
    idxCache_[0] = '\1';
    nameCache_[0] = '\1';
    screen_.markAllDirty();
}

NavIntent VfoView::onDstEditEvent(const Event &e)
{
    if (e.kind == EvKind::Encoder) {
        /* Knob moves the cursor on arrow-less radios, cycles elsewhere. */
        if (kbdHasArrows())
            dstCycle(e.encoder);
        else
            dstMove(e.encoder > 0 ? +1 : -1);
    } else if (e.kind == EvKind::Key) {
        const uint32_t k = e.keys;
        if ((k & KEY_ENTER) != 0u)
            endDstEdit(true);
        else if ((k & KEY_ESC) != 0u)
            endDstEdit(false);
        else if ((k & KEY_HASH) != 0u) {
            /* # clears the destination and exits (classic parity). */
            dst_.clear();
            endDstEdit(true);
        } else if ((k & KBD_CHAR_MASK) != 0u) {
            /* Numeric-keypad multi-tap (# handled above); '*' backspaces. */
            const uint8_t ki =
                static_cast<uint8_t>(__builtin_ctz(k & KBD_CHAR_MASK));
            if (ki == 10u)
                dst_.backspace();
            else
                dst_.tapKey(ki, getTick());
            renderDstEdit();
            announceDstChar();
        } else if ((k & KEY_UP) != 0u)
            dstCycle(+1);
        else if ((k & KEY_DOWN) != 0u)
            dstCycle(-1);
        else if ((k & KEY_LEFT) != 0u)
            dstMove(-1);
        else if ((k & KEY_RIGHT) != 0u)
            dstMove(+1);
    }
    return NavIntent::none();
}

void VfoView::announceFreq()
{
    if (state.settings.vpLevel < vpLow)
        return;
    vp_flush();
    vp_announceFrequencies(state.channel.rx_frequency,
                           state.channel.tx_frequency,
                           vp_getVoiceLevelQueueFlags());
    vp_play();
}

void VfoView::announceVfoState()
{
    /* In memory mode speak the full channel summary; when tuning speak the
     * frequency pair and mode. Mirrors the classic VFO/MEM announcements. */
    if (state.settings.vpLevel < vpLow)
        return;

    const enum vpQueueFlags flags = vp_getVoiceLevelQueueFlags();
    vp_flush();
    if (state.tuner_mode != VFO) {
        vp_announceChannelSummary(
            &state.channel, (uint16_t)(state.channel_index + 1), 0, vpAllInfo);
    } else {
        vp_announceFrequencies(state.channel.rx_frequency,
                               state.channel.tx_frequency, flags);
        vp_announceRadioMode(state.channel.mode, flags);
    }
    vp_play();
}

void VfoView::announce()
{
    announceVfoState();
}

} // namespace ortxui
