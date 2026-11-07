/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/aprs_qsy.h"
#include "interfaces/platform.h"
#include "rtx/rtx.h"
#include "hwconfig.h"
#include <stddef.h>

/* Standard repeater shifts by output frequency, US band plan: the table
 * behind Kenwood's automatic repeater offset. */
static const struct {
    uint32_t lo;
    uint32_t hi;
    int32_t shift;
} SHIFTS[] = {
    { 145100000, 145499999, -600000 },  /* 2 m     */
    { 146000000, 146399999, 600000 },   /* 2 m     */
    { 146600000, 146999999, -600000 },  /* 2 m     */
    { 147000000, 147399999, 600000 },   /* 2 m     */
    { 147600000, 147999999, -600000 },  /* 2 m     */
    { 223850000, 224999999, -1600000 }, /* 1.25 m  */
    { 442000000, 444999999, 5000000 },  /* 70 cm   */
    { 447000000, 449999999, -5000000 }, /* 70 cm   */
};

static bool inBand(const uint32_t freq)
{
    const hwInfo_t *hw = platform_getHwInfo();

    if (hw->vhf_band && (freq >= hw->vhf_minFreq * 1000000u)
        && (freq <= hw->vhf_maxFreq * 1000000u))
        return true;

    if (hw->uhf_band && (freq >= hw->uhf_minFreq * 1000000u)
        && (freq <= hw->uhf_maxFreq * 1000000u))
        return true;

    return false;
}

int32_t aprsQsy_offset(const struct aprsFreq *f)
{
    if (f->flags & APRS_FREQ_HAS_OFFSET)
        return f->offset;

    for (size_t i = 0; i < sizeof(SHIFTS) / sizeof(SHIFTS[0]); i++) {
        if ((f->freq >= SHIFTS[i].lo) && (f->freq <= SHIFTS[i].hi))
            return SHIFTS[i].shift;
    }

    return 0;
}

uint8_t aprsQsy_toneIndex(uint16_t hz)
{
    for (uint8_t i = 0; i < CTCSS_FREQ_NUM; i++) {
        if ((ctcss_tone[i] / 10) == hz)
            return i;
    }

    return CTCSS_FREQ_NUM;
}

enum aprsQsyResult aprsQsy_tune(const struct aprsFreq *f, channel_t *ch)
{
    uint8_t mode;
    switch (f->mode) {
        case APRS_FREQ_FM:
            mode = OPMODE_FM;
            break;
#ifdef CONFIG_M17
        case APRS_FREQ_M17:
            mode = OPMODE_M17;
            break;
#endif
        default:
            return APRS_QSY_MODE;
    }

    /* Tones only matter in FM */
    uint8_t tone = CTCSS_FREQ_NUM;
    if (mode == OPMODE_FM) {
        switch (f->toneType) {
            case APRS_TONE_NONE:
                break;
            case APRS_TONE_ENCODE:
            case APRS_TONE_SQUELCH:
                tone = aprsQsy_toneIndex(f->tone);
                if (tone == CTCSS_FREQ_NUM)
                    return APRS_QSY_TONE;
                break;
            default:
                return APRS_QSY_TONE;
        }
    }

    const uint32_t tx = (uint32_t)((int64_t)f->freq + aprsQsy_offset(f));
    if (!inBand(f->freq) || !inBand(tx))
        return APRS_QSY_BAND;

    if (ch == NULL)
        return APRS_QSY_OK;

    ch->mode = mode;
    ch->rx_frequency = f->freq;
    ch->tx_frequency = tx;
    if (mode == OPMODE_M17) {
        uint8_t can = (f->flags & APRS_FREQ_HAS_CAN) ? f->can : 0;
        ch->m17.rxCan = can;
        ch->m17.txCan = can;
        return APRS_QSY_OK;
    }

    ch->bandwidth = (f->flags & APRS_FREQ_NARROW) ? BW_12_5 : BW_25;
    ch->fm.txToneEn = (tone != CTCSS_FREQ_NUM) ? 1 : 0;
    ch->fm.rxToneEn = (f->toneType == APRS_TONE_SQUELCH) ? 1 : 0;
    if (tone != CTCSS_FREQ_NUM) {
        ch->fm.txTone = tone;
        ch->fm.rxTone = tone;
    }

    return APRS_QSY_OK;
}
