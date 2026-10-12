/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/dsp.h"

int16_t dsp_dcBlockFilter(struct dcBlock *dcb, int16_t sample)
{
    /*
     * Implementation of a fixed-point DC block filter with noise shaping,
     * ensuring zero DC component at the output.
     *
     * The pole is configurable via dcb->pole (fixed-point, scaled by 32768).
     * A pole of zero uses the default value 0.995 (K = 164), which is
     * appropriate for microphone audio. For M17 baseband (4800 Bd 4FSK),
     * a slower pole such as 0.9995 (K = 16) reduces baseline wander.
     *
     * Code adapted from https://dspguru.com/dsp/tricks/fixed-point-dc-blocking-filter-with-noise-shaping/
     */
    int32_t K;

    if (dcb->pole == 0)
        K = 164;               // default pole 0.995: 32768 * (1 - 0.995)
    else
        K = 32768 - dcb->pole; // K = 32768 * (1 - pole/32768)

    dcb->accum -= dcb->prevIn;
    dcb->prevIn = static_cast<int32_t>(sample) << 15;
    dcb->accum += dcb->prevIn;
    dcb->accum -= K * dcb->prevOut;
    dcb->prevOut = dcb->accum >> 15;

    return static_cast<int16_t>(dcb->prevOut);
}

bool dsp_decimator(struct decimatorState *state, int16_t *sample,
                   uint16_t ratio)
{
    state->accumulator += *sample;
    state->count++;
    if (state->count >= ratio) {
        // Integer division truncates toward zero; this is intentional as
        // rounding is unnecessary for audio signal averaging.
        *sample = state->accumulator / ratio;
        state->accumulator = 0;
        state->count = 0;
        return true;
    }

    return false;
}
