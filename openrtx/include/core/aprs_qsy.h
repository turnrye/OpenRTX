/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APRS_QSY_H
#define APRS_QSY_H

#include "core/cps.h"
#include "protocols/APRS/packet.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum aprsQsyResult {
    APRS_QSY_OK = 0,
    APRS_QSY_MODE, /**< Worked in a mode this radio does not have.    */
    APRS_QSY_TONE, /**< Needs DCS, a tone burst or an unknown tone.   */
    APRS_QSY_BAND, /**< Receive or transmit outside the radio's bands. */
};

/**
 * @brief The offset to transmit with: the one sent, or the band's standard
 * shift by the US band plan, as Kenwood's automatic repeater offset.
 *
 * @param f: the announcement.
 * @return Hz added to the frequency to transmit; 0 for simplex.
 */
int32_t aprsQsy_offset(const struct aprsFreq *f);

/**
 * @brief The standard CTCSS tone a whole-Hz tone stands for.
 *
 * @param hz: the tone as sent, "107" for 107.2.
 * @return index into ctcss_tone[], or CTCSS_FREQ_NUM if none.
 */
uint8_t aprsQsy_toneIndex(uint16_t hz);

/**
 * @brief Tune a channel to an announced frequency.
 *
 * Mode, frequencies, tone and FM bandwidth come from the announcement;
 * everything else stays as the channel had it.
 *
 * @param f: the announcement.
 * @param ch: the channel to tune, untouched unless APRS_QSY_OK; NULL only
 * checks.
 * @return APRS_QSY_OK, or why this radio cannot work the frequency.
 */
enum aprsQsyResult aprsQsy_tune(const struct aprsFreq *f, channel_t *ch);

#ifdef __cplusplus
}
#endif

#endif /* APRS_QSY_H */
