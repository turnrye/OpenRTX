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
     * Filter pole set at 0.995
     *
     * Code adapted from https://dspguru.com/dsp/tricks/fixed-point-dc-blocking-filter-with-noise-shaping/
     */
    dcb->accum -= dcb->prevIn;
    dcb->prevIn = static_cast<int32_t>(sample) * 32768;
    dcb->accum += dcb->prevIn;
    dcb->accum -= 164 * dcb->prevOut; // 32768.0 * (1.0 - pole)
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

#include <math.h>

void dsp_highPassInit(struct biquad *bq, float cutoff, float sampleRate)
{
    // RBJ cookbook high-pass, Q = 1/sqrt(2) (Butterworth)
    const float w0    = 2.0f * (float)M_PI * cutoff / sampleRate;
    const float cw    = cosf(w0);
    const float alpha = sinf(w0) / (2.0f * 0.70710678f);
    const float a0    = 1.0f + alpha;

    bq->b0 = ((1.0f + cw) / 2.0f) / a0;
    bq->b1 = -(1.0f + cw) / a0;
    bq->b2 = ((1.0f + cw) / 2.0f) / a0;
    bq->a1 = (-2.0f * cw) / a0;
    bq->a2 = (1.0f - alpha) / a0;
    bq->z1 = 0.0f;
    bq->z2 = 0.0f;
}

void dsp_agcDefaults(struct agcParams *params)
{
    params->targetDb          = -21.0f;
    params->minGainDb         = -12.0f;
    params->maxGainDb         = 24.0f;
    params->attackDbPerBlk    = 1.5f;   // ~75 dB/s with 20 ms blocks
    params->releaseDbPerBlk   = 0.08f;  // ~4 dB/s
    params->fastDownDb        = 6.0f;
    params->gateAboveFloor    = 9.0f;
    params->gateMinDb         = -65.0f;
    params->floorRiseDbPerBlk = 0.02f;  // ~1 dB/s
    params->kneeDb            = -6.0f;
    params->ceilingDb         = -1.0f;
}

void dsp_agcInit(struct agcState *agc, const struct agcParams *params)
{
    agc->p        = *params;
    agc->gainDb   = 0.0f;
    agc->levelDb  = params->targetDb;
    agc->floorDb  = params->gateMinDb;
    agc->gateOpen = false;
    agc->acquire  = 50;     // 1 s of 20 ms blocks
}

float dsp_softLimit(float x, float knee, float ceiling)
{
    const float a = fabsf(x);
    if(a <= knee)
        return x;

    const float span = ceiling - knee;
    const float y    = knee + span * tanhf((a - knee) / span);
    return (x < 0.0f) ? -y : y;
}

static inline float dbToLin(float db)
{
    return powf(10.0f, db / 20.0f);
}

void dsp_agcProcess(struct agcState *agc, float *block, size_t len)
{
    if(len == 0)
        return;

    // Level of the block before gain
    float power = 0.0f;
    for(size_t i = 0; i < len; i++)
        power += block[i] * block[i];

    const float blkDb = 10.0f * log10f((power / (float)len) + 1e-12f);

    // Noise floor: follows quiet blocks at once, rises slowly otherwise
    if(blkDb < agc->floorDb)
        agc->floorDb = blkDb;
    else
        agc->floorDb += agc->p.floorRiseDbPerBlk;

    float gate = agc->floorDb + agc->p.gateAboveFloor;
    if(gate < agc->p.gateMinDb)
        gate = agc->p.gateMinDb;

    agc->gateOpen = (blkDb > gate);

    const float prevGainDb = agc->gainDb;
    if(agc->gateOpen) {
        // Speech level: rise fast, fall slowly, so the gain follows phrases
        // rather than individual syllables
        if(blkDb > agc->levelDb)
            agc->levelDb += 0.5f * (blkDb - agc->levelDb);
        else
            agc->levelDb += 0.05f * (blkDb - agc->levelDb);

        float want = agc->p.targetDb - agc->levelDb;
        if(want > agc->p.maxGainDb) want = agc->p.maxGainDb;
        if(want < agc->p.minGainDb) want = agc->p.minGainDb;

        // A block that would overshoot the target at the current gain is
        // pulled down at once, up to the attack rate
        const float over = (blkDb + agc->gainDb) - agc->p.targetDb;
        if((agc->p.fastDownDb > 0.0f) && (over > agc->p.fastDownDb)
           && (want > agc->gainDb - over))
            want = agc->gainDb - over;

        if(want < agc->gainDb) {
            float step = agc->gainDb - want;
            if(step > agc->p.attackDbPerBlk) step = agc->p.attackDbPerBlk;
            agc->gainDb -= step;
        } else {
            float step  = want - agc->gainDb;
            float limit = agc->p.releaseDbPerBlk;
            if(agc->acquire > 0) limit *= 10.0f;
            if(step > limit) step = limit;
            agc->gainDb += step;
        }

        if(agc->acquire > 0)
            agc->acquire--;
    }

    // Ramp the gain across the block, then limit
    const float g0    = dbToLin(prevGainDb);
    const float g1    = dbToLin(agc->gainDb);
    const float dg    = (g1 - g0) / (float)len;
    const float knee  = dbToLin(agc->p.kneeDb);
    const float ceil  = dbToLin(agc->p.ceilingDb);

    for(size_t i = 0; i < len; i++) {
        const float g = g0 + dg * (float)(i + 1);
        block[i] = dsp_softLimit(block[i] * g, knee, ceil);
    }
}
