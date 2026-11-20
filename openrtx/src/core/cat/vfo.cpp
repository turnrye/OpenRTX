/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * CAT commands of the tuned channel: frequency, mode, repeater shift and
 * offset, tones.
 */

#include "core/cat/commands.hpp"
#include "core/cps.h"
#include "core/utils.h"
#include "hwconfig.h"
#include "rtx/rtx.h"
#include <cerrno>

/**
 * Repeater shift, numbered as OS and IF number it.
 */
enum class Shift : uint8_t {
    SIMPLEX = 0,
    PLUS = 1,
    MINUS = 2,
};

/**
 * Tone mode, numbered as CT and IF number it.
 */
enum class ToneMode : uint8_t {
    OFF = 0,
    ENC_DEC = 1,
    ENC = 2,
};

/**
 * Modes of ZM.
 */
enum class ZmMode : uint8_t {
    FM = 0,
    NFM = 1,
    M17 = 2,
};

// Mode characters of MD and IF
static constexpr char MODE_FM = '4';
static constexpr char MODE_NFM = 'B';

// EX menus of the repeater offset, one per band on the FT-991A (28, 50, 144
// and 430 MHz); OpenRTX keeps one offset whatever the band
static constexpr uint32_t EX_OFFSET_FIRST = 80;
static constexpr uint32_t EX_OFFSET_LAST = 83;

// EX menu of the CAT time-out
static constexpr uint32_t EX_CAT_TOT_MENU = 32;

// Repeater offset OS gives a simplex channel, in Hz
static freq_t defaultOffset = 600000;

// EX032 value; Hamlib sets it when it opens an FT-991
static uint8_t catTimeout = 1;

/**
 * Get the repeater shift of a channel.
 *
 * @param ch: channel.
 * @return shift of the TX frequency from the RX one.
 */
static Shift shiftOf(const channel_t &ch)
{
    if (ch.tx_frequency > ch.rx_frequency)
        return Shift::PLUS;
    if (ch.tx_frequency < ch.rx_frequency)
        return Shift::MINUS;

    return Shift::SIMPLEX;
}

/**
 * Get the repeater offset of a channel: its own when shifted, otherwise the
 * one OS would apply.
 *
 * @param ch: channel.
 * @return offset in Hz.
 */
static freq_t offsetOf(const channel_t &ch)
{
    switch (shiftOf(ch)) {
        case Shift::PLUS:
            return ch.tx_frequency - ch.rx_frequency;
        case Shift::MINUS:
            return ch.rx_frequency - ch.tx_frequency;
        default:
            return defaultOffset;
    }
}

/**
 * Get the TX frequency of a shifted RX frequency.
 *
 * @param rx: RX frequency in Hz.
 * @param shift: repeater shift.
 * @param offset: repeater offset in Hz.
 * @return TX frequency in Hz.
 */
static freq_t shiftedTx(const freq_t rx, const Shift shift, const freq_t offset)
{
    switch (shift) {
        case Shift::PLUS:
            return rx + offset;
        case Shift::MINUS:
            return rx - offset;
        default:
            return rx;
    }
}

/**
 * Tune a channel, if the radio can transmit and receive on both frequencies.
 *
 * @param ch: channel to tune.
 * @param rx: RX frequency in Hz.
 * @param tx: TX frequency in Hz.
 * @return 0 on success, -ENOTSUP for a frequency out of the radio's bands.
 */
static int tune(channel_t &ch, const freq_t rx, const freq_t tx)
{
    bool inBand = freqInBand(rx) && freqInBand(tx);
    if (inBand == false)
        return -ENOTSUP;

    ch.rx_frequency = rx;
    ch.tx_frequency = tx;
    return 0;
}

/**
 * Get the mode character of a channel.
 *
 * @param ch: channel.
 * @return mode character, 0 for a mode the Yaesu commands do not carry.
 */
static char modeOf(const channel_t &ch)
{
    if (ch.mode != OPMODE_FM)
        return 0;

    return (ch.bandwidth == BW_12_5) ? MODE_NFM : MODE_FM;
}

/**
 * Get the tone mode of a channel. Decoding without encoding has no CT value,
 * and reads as encoding and decoding.
 *
 * @param ch: channel.
 * @return tone mode.
 */
static ToneMode toneModeOf(const channel_t &ch)
{
    if ((ch.fm.txToneEn != 0) && (ch.fm.rxToneEn == 0))
        return ToneMode::ENC;
    if ((ch.fm.txToneEn != 0) || (ch.fm.rxToneEn != 0))
        return ToneMode::ENC_DEC;

    return ToneMode::OFF;
}

/**
 * Get a copy of the current channel.
 *
 * @return current channel.
 */
static channel_t currentChannel()
{
    return cat_lockState([](const state_t &s) { return s.channel; });
}

/**
 * Check that a command addresses the main receiver, the fixed '0' that
 * follows the letters of MD, NA, OS, SH and CT.
 *
 * @param req: request.
 * @return true if the first parameter is '0'.
 */
static bool mainReceiver(const CatRequest &req)
{
    return (req.len >= 1) && (req.args[0] == '0');
}

/**
 * Change the current channel when it is an FM channel; the change is not a
 * tuning change.
 *
 * @param edit: function changing the channel.
 * @return 0 on success, -ENOTSUP if the channel is not FM.
 */
template <typename Function>
static int editFmChannel(Function edit)
{
    return cat_editChannel(false, [&edit](channel_t &ch) {
        if (ch.mode != OPMODE_FM)
            return -ENOTSUP;

        edit(ch);
        return 0;
    });
}

int cat_cmdFA(const CatRequest &req, CatAnswer &answer)
{
    uint32_t freq;

    if (req.len == 0) {
        answer.append("FA", 2);
        answer.appendNumber(currentChannel().rx_frequency, 9);
        return 0;
    }

    if ((req.len != 9) || !cat_parseNumber(req.args, req.len, freq))
        return -EINVAL;

    // The channel keeps its repeater shift and offset
    return cat_editChannel(true, [freq](channel_t &ch) {
        return tune(ch, freq, shiftedTx(freq, shiftOf(ch), offsetOf(ch)));
    });
}

int cat_cmdFB(const CatRequest &req, CatAnswer &answer)
{
    uint32_t freq;

    if (req.len == 0) {
        answer.append("FB", 2);
        answer.appendNumber(currentChannel().tx_frequency, 9);
        return 0;
    }

    if ((req.len != 9) || !cat_parseNumber(req.args, req.len, freq))
        return -EINVAL;

    return cat_editChannel(true, [freq](channel_t &ch) {
        return tune(ch, ch.rx_frequency, freq);
    });
}

int cat_cmdBS(const CatRequest &req, CatAnswer &answer)
{
    (void)answer;
    uint32_t band;

    if ((req.len != 2) || !cat_parseNumber(req.args, req.len, band))
        return -EINVAL;

    return 0;
}

int cat_cmdFT(const CatRequest &req, CatAnswer &answer)
{
    if (req.len == 0) {
        answer.append("FT0", 3);
        return 0;
    }

    bool vfo = (req.args[0] == '2') || (req.args[0] == '3');
    if ((req.len != 1) || (vfo == false))
        return -EINVAL;

    return 0;
}

int cat_cmdMD(const CatRequest &req, CatAnswer &answer)
{
    if (mainReceiver(req) == false)
        return -EINVAL;

    if (req.len == 1) {
        char mode = modeOf(currentChannel());
        if (mode == 0)
            return -ENOTSUP;

        answer.append("MD0", 3);
        answer.append(mode);
        return 0;
    }

    bool fm = (req.args[1] == MODE_FM) || (req.args[1] == MODE_NFM);
    if ((req.len != 2) || (fm == false))
        return -EINVAL;

    uint8_t bandwidth = (req.args[1] == MODE_NFM) ? BW_12_5 : BW_25;
    return cat_editChannel(false, [bandwidth](channel_t &ch) {
        ch.mode = OPMODE_FM;
        ch.bandwidth = bandwidth;
        return 0;
    });
}

int cat_cmdNA(const CatRequest &req, CatAnswer &answer)
{
    if (mainReceiver(req) == false)
        return -EINVAL;

    if (req.len == 1) {
        channel_t ch = currentChannel();
        if (ch.mode != OPMODE_FM)
            return -ENOTSUP;

        answer.append("NA0", 3);
        answer.append((ch.bandwidth == BW_12_5) ? '1' : '0');
        return 0;
    }

    bool narrow = (req.args[1] == '1');
    if ((req.len != 2) || ((narrow == false) && (req.args[1] != '0')))
        return -EINVAL;

    return editFmChannel(
        [narrow](channel_t &ch) { ch.bandwidth = narrow ? BW_12_5 : BW_25; });
}

int cat_cmdSH(const CatRequest &req, CatAnswer &answer)
{
    uint32_t width;

    if (mainReceiver(req) == false)
        return -EINVAL;

    if (req.len == 1) {
        answer.append("SH000", 5);
        return 0;
    }

    if ((req.len != 3) || !cat_parseNumber(&req.args[1], 2, width))
        return -EINVAL;

    return 0;
}

int cat_cmdIF(const CatRequest &req, CatAnswer &answer)
{
    channel_t ch = currentChannel();
    char mode = modeOf(ch);

    if (req.len != 0)
        return -EINVAL;
    if (mode == 0)
        return -ENOTSUP;

    // Memory channel, frequency, clarifier (off), mode, VFO, tone mode, fixed
    // field, repeater shift
    answer.append("IF000", 5);
    answer.appendNumber(ch.rx_frequency, 9);
    answer.append("+000000", 7);
    answer.append(mode);
    answer.append('0');
    answer.appendNumber(static_cast<uint32_t>(toneModeOf(ch)), 1);
    answer.append("00", 2);
    answer.appendNumber(static_cast<uint32_t>(shiftOf(ch)), 1);
    return 0;
}

int cat_cmdOS(const CatRequest &req, CatAnswer &answer)
{
    uint32_t value;

    if (mainReceiver(req) == false)
        return -EINVAL;

    if (req.len == 1) {
        answer.append("OS0", 3);
        answer.appendNumber(static_cast<uint32_t>(shiftOf(currentChannel())),
                            1);
        return 0;
    }

    bool valid = (req.len == 2) && cat_parseNumber(&req.args[1], 1, value)
              && (value <= static_cast<uint32_t>(Shift::MINUS));
    if (valid == false)
        return -EINVAL;

    Shift shift = static_cast<Shift>(value);
    return cat_editChannel(true, [shift](channel_t &ch) {
        freq_t rx = ch.rx_frequency;

        return tune(ch, rx, shiftedTx(rx, shift, offsetOf(ch)));
    });
}

/**
 * Set the repeater offset, retuning the current channel when it is shifted.
 * On a simplex channel only the offset the next OS applies changes.
 *
 * @param offset: offset in Hz.
 * @return 0 on success, -ENOTSUP if the retuned channel would leave the
 * radio's bands.
 */
static int setOffset(const freq_t offset)
{
    if (shiftOf(currentChannel()) == Shift::SIMPLEX) {
        defaultOffset = offset;
        return 0;
    }

    return cat_editChannel(true, [offset](channel_t &ch) {
        Shift shift = shiftOf(ch);

        if (shift != Shift::SIMPLEX) {
            freq_t tx = shiftedTx(ch.rx_frequency, shift, offset);
            int err = tune(ch, ch.rx_frequency, tx);
            if (err < 0)
                return err;
        }

        defaultOffset = offset;
        return 0;
    });
}

int cat_cmdEX(const CatRequest &req, CatAnswer &answer)
{
    uint32_t menu;
    uint32_t value;

    if ((req.len < 3) || !cat_parseNumber(req.args, 3, menu))
        return -EINVAL;

    if (menu == EX_CAT_TOT_MENU) {
        if (req.len == 3) {
            answer.append("EX032", 5);
            answer.appendNumber(catTimeout, 1);
            return 0;
        }

        if ((req.len != 4) || !cat_parseNumber(&req.args[3], 1, value)
            || (value > 3))
            return -EINVAL;

        catTimeout = value;
        return 0;
    }

    bool offsetMenu = (menu >= EX_OFFSET_FIRST) && (menu <= EX_OFFSET_LAST);
    if (offsetMenu == false)
        return -EINVAL;

    // Four digits of kHz, five for 430 MHz
    size_t digits = (menu == EX_OFFSET_LAST) ? 5 : 4;

    if (req.len == 3) {
        answer.append("EX", 2);
        answer.appendNumber(menu, 3);
        answer.appendNumber(offsetOf(currentChannel()) / 1000, digits);
        return 0;
    }

    // Hamlib sends four digits for every band
    size_t len = req.len - 3;
    if ((len > digits) || !cat_parseNumber(&req.args[3], len, value))
        return -EINVAL;

    return setOffset(value * 1000);
}

int cat_cmdZO(const CatRequest &req, CatAnswer &answer)
{
    uint32_t offset;

    if (req.len == 0) {
        answer.append("ZO", 2);
        answer.appendNumber(offsetOf(currentChannel()), 9);
        return 0;
    }

    if (cat_parseNumber(req.args, req.len, offset) == false)
        return -EINVAL;

    return setOffset(offset);
}

int cat_cmdCN(const CatRequest &req, CatAnswer &answer)
{
    uint32_t tone;

    // Only CTCSS (P2 = 0), no DCS
    bool ctcss = (req.len >= 2) && (req.args[0] == '0') && (req.args[1] == '0');
    if (ctcss == false)
        return -EINVAL;

    if (req.len == 2) {
        channel_t ch = currentChannel();
        if (ch.mode != OPMODE_FM)
            return -ENOTSUP;

        answer.append("CN00", 4);
        answer.appendNumber(ch.fm.txTone, 3);
        return 0;
    }

    bool valid = (req.len == 5) && cat_parseNumber(&req.args[2], 3, tone)
              && (tone < CTCSS_FREQ_NUM);
    if (valid == false)
        return -EINVAL;

    return editFmChannel([tone](channel_t &ch) {
        ch.fm.txTone = tone;
        ch.fm.rxTone = tone;
    });
}

int cat_cmdCT(const CatRequest &req, CatAnswer &answer)
{
    uint32_t value;

    if (mainReceiver(req) == false)
        return -EINVAL;

    if (req.len == 1) {
        channel_t ch = currentChannel();
        if (ch.mode != OPMODE_FM)
            return -ENOTSUP;

        answer.append("CT0", 3);
        answer.appendNumber(static_cast<uint32_t>(toneModeOf(ch)), 1);
        return 0;
    }

    bool valid = (req.len == 2) && cat_parseNumber(&req.args[1], 1, value)
              && (value <= static_cast<uint32_t>(ToneMode::ENC));
    if (valid == false)
        return -EINVAL;

    ToneMode mode = static_cast<ToneMode>(value);
    return editFmChannel([mode](channel_t &ch) {
        ch.fm.txToneEn = (mode != ToneMode::OFF);
        ch.fm.rxToneEn = (mode == ToneMode::ENC_DEC);
    });
}

int cat_cmdZM(const CatRequest &req, CatAnswer &answer)
{
    uint32_t value;

    if (req.len == 0) {
        channel_t ch = currentChannel();
        ZmMode mode = ZmMode::FM;
        if (ch.mode == OPMODE_M17)
            mode = ZmMode::M17;
        else if (ch.bandwidth == BW_12_5)
            mode = ZmMode::NFM;

        answer.append("ZM", 2);
        answer.appendNumber(static_cast<uint32_t>(mode), 2);
        return 0;
    }

    bool valid = (req.len == 2) && cat_parseNumber(req.args, req.len, value)
              && (value <= static_cast<uint32_t>(ZmMode::M17));
    if (valid == false)
        return -EINVAL;

    ZmMode mode = static_cast<ZmMode>(value);
#ifndef CONFIG_M17
    if (mode == ZmMode::M17)
        return -ENOTSUP;
#endif

    return cat_editChannel(false, [mode](channel_t &ch) {
        if (mode == ZmMode::M17) {
            ch.mode = OPMODE_M17;
        } else {
            ch.mode = OPMODE_FM;
            ch.bandwidth = (mode == ZmMode::NFM) ? BW_12_5 : BW_25;
        }
        return 0;
    });
}
