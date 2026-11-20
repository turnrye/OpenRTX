/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CAT_COMMANDS_H
#define CAT_COMMANDS_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include "core/cat/CatAnswer.hpp"
#include "core/state.h"
#include <cstddef>
#include <cstdint>
#include <pthread.h>

/*
 * CAT command handlers, grouped by what they control: the tuned channel
 * (vfo.cpp), TX power and receiver levels (levels.cpp), M17 settings
 * (m17.cpp) and the radio itself (system.cpp). cat.cpp holds the table that
 * dispatches a command to its handler.
 *
 * A handler returns 0 on success, with the answer to a read in answer, or
 * -EINVAL for malformed parameters and -ENOTSUP for a command the radio cannot
 * carry out; either error answers "?;".
 */

/**
 * A CAT command to execute.
 */
struct CatRequest {
    const char *args; ///< Parameters, after the two letters of the command.
    size_t len;       ///< Number of parameter characters, 0 for a plain read.
    long long now;    ///< Current time, in milliseconds.
};

/**
 * Handler of a CAT command.
 */
using CatHandler = int (*)(const CatRequest &req, CatAnswer &answer);

/**
 * Parse an unsigned decimal number.
 *
 * @param text: digits.
 * @param len: number of digits, 1 to 9.
 * @param value: parsed number.
 * @return true if text holds only digits and len is in range.
 */
bool cat_parseNumber(const char *text, const size_t len, uint32_t &value);

/**
 * Have the radio apply a change to the radio state; whichever UI runs redraws
 * from the state on its own. Call with state_mutex held.
 */
void cat_stateChanged();

/**
 * Record a change to the current channel, with state_mutex held: a tuning
 * change puts the radio in VFO mode, as CAT always tunes the VFO, and in VFO
 * mode the VFO copy follows every change. Then as cat_stateChanged().
 *
 * @param s: radio state.
 * @param tuning: whether the frequencies changed.
 */
void cat_channelChanged(state_t &s, const bool tuning);

/**
 * Run a function on the radio state, holding state_mutex.
 *
 * @param fn: function taking a state_t reference.
 * @return what fn returned.
 */
template <typename Function>
auto cat_lockState(Function fn)
{
    pthread_mutex_lock(&state_mutex);
    auto ret = fn(state);
    pthread_mutex_unlock(&state_mutex);

    return ret;
}

/**
 * Change the current channel, holding state_mutex, and have the radio apply
 * the change.
 *
 * @param tuning: whether the change is to the frequencies.
 * @param edit: function taking a channel_t reference; it returns 0 when it
 * changed the channel, or a negative error code when it left it untouched.
 * @return what edit returned.
 */
template <typename Function>
int cat_editChannel(const bool tuning, Function edit)
{
    return cat_lockState([tuning, &edit](state_t &s) {
        int ret = edit(s.channel);
        if (ret == 0)
            cat_channelChanged(s, tuning);

        return ret;
    });
}

/**
 * Change the settings, holding state_mutex, and have the radio apply the
 * change.
 *
 * @param edit: function taking a settings_t reference.
 */
template <typename Function>
void cat_editSettings(Function edit)
{
    cat_lockState([&edit](state_t &s) {
        edit(s.settings);
        cat_stateChanged();
        return 0;
    });
}

/**
 * BS: band select, accepted and ignored. Hamlib sends it before FA.
 */
int cat_cmdBS(const CatRequest &req, CatAnswer &answer);

/**
 * CN: CTCSS tone number, OpenRTX's own index. DCS is not supported.
 */
int cat_cmdCN(const CatRequest &req, CatAnswer &answer);

/**
 * CT: tone mode, 0 off, 1 encode and decode, 2 encode.
 */
int cat_cmdCT(const CatRequest &req, CatAnswer &answer);

/**
 * EX: menus 032 (CAT time-out, kept but unused) and 080-083 (the repeater
 * offset in kHz, one per band on the FT-991A, one for all bands here).
 */
int cat_cmdEX(const CatRequest &req, CatAnswer &answer);

/**
 * FA: RX frequency in Hz. A set keeps the repeater shift.
 */
int cat_cmdFA(const CatRequest &req, CatAnswer &answer);

/**
 * FB: TX frequency in Hz, as VFO-B is in split.
 */
int cat_cmdFB(const CatRequest &req, CatAnswer &answer);

/**
 * FT: TX VFO, accepted and ignored: FB is the TX frequency.
 */
int cat_cmdFT(const CatRequest &req, CatAnswer &answer);

/**
 * IF: information record of the current channel.
 */
int cat_cmdIF(const CatRequest &req, CatAnswer &answer);

/**
 * MD: mode, FM or FM narrow. A set switches any mode to FM.
 */
int cat_cmdMD(const CatRequest &req, CatAnswer &answer);

/**
 * NA: FM narrow. Hamlib sends it after MD.
 */
int cat_cmdNA(const CatRequest &req, CatAnswer &answer);

/**
 * OS: repeater shift, keeping the channel's offset or applying the one of
 * EX080-EX083 and ZO.
 */
int cat_cmdOS(const CatRequest &req, CatAnswer &answer);

/**
 * SH: filter width, always the default. Hamlib reads it with MD.
 */
int cat_cmdSH(const CatRequest &req, CatAnswer &answer);

/**
 * ZM: mode in OpenRTX's table, 00 FM, 01 FM narrow, 02 M17.
 */
int cat_cmdZM(const CatRequest &req, CatAnswer &answer);

/**
 * ZO: repeater offset, in Hz.
 */
int cat_cmdZO(const CatRequest &req, CatAnswer &answer);

/**
 * PC: TX power in whole watts.
 */
int cat_cmdPC(const CatRequest &req, CatAnswer &answer);

/**
 * SM: S-meter, on the FT-991A's 0-255 scale.
 */
int cat_cmdSM(const CatRequest &req, CatAnswer &answer);

/**
 * SQ: squelch, OpenRTX's 0-15 scaled to 0-100.
 */
int cat_cmdSQ(const CatRequest &req, CatAnswer &answer);

/**
 * ZP: TX power in mW.
 */
int cat_cmdZP(const CatRequest &req, CatAnswer &answer);

/**
 * ZC: callsign, the M17 source.
 */
int cat_cmdZC(const CatRequest &req, CatAnswer &answer);

/**
 * ZD: M17 destination, ALL for the broadcast.
 */
int cat_cmdZD(const CatRequest &req, CatAnswer &answer);

/**
 * ZN: M17 channel access number.
 */
int cat_cmdZN(const CatRequest &req, CatAnswer &answer);

/**
 * AI: auto information, always off.
 */
int cat_cmdAI(const CatRequest &req, CatAnswer &answer);

/**
 * ID: identification.
 */
int cat_cmdID(const CatRequest &req, CatAnswer &answer);

/**
 * PS: power switch, always on. Power off is refused: CAT could not turn the
 * radio back on.
 */
int cat_cmdPS(const CatRequest &req, CatAnswer &answer);

/**
 * TX: 0 receive, 1 transmit, 2 (read only) the radio's own PTT.
 */
int cat_cmdTX(const CatRequest &req, CatAnswer &answer);

/**
 * ZR: battery charge, percent.
 */
int cat_cmdZR(const CatRequest &req, CatAnswer &answer);

/**
 * ZV: firmware version.
 */
int cat_cmdZV(const CatRequest &req, CatAnswer &answer);

#endif // CAT_COMMANDS_H
