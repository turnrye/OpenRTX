/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DSP_H
#define DSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * This header contains various DSP utilities which can be used to condition
 * input or output signals when implementing digital modes on OpenRTX.
 */

/**
 * Reset the state variables of a DSP object.
 *
 * @param state: state variable.
 */
#define dsp_resetState(state) memset(&state, 0x00, sizeof(state))

/**
 * Data structure holding the internal state of a DC blocking filter.
 */
struct dcBlock {
    int32_t accum;
    int32_t prevIn;
    int32_t prevOut;
};

/**
 * Run a single step of the DC blocking filter.
 *
 * @param dcb: pointer to DC filter state.
 * @param sample: input sample.
 * @return filtered sample
 */
int16_t dsp_dcBlockFilter(struct dcBlock *dcb, int16_t sample);

/**
 * Remove the DC offset from a collection of samples, processing data in-place.
 *
 * @param state: pointer to the data structure containing the filter state.
 * @param buffer: buffer containing the audio samples.
 * @param length: number of samples contained in the buffer.
 */
static inline void dsp_removeDcOffset(struct dcBlock *dcb, int16_t *buffer,
                                      const size_t length)
{
    for (size_t i = 0; i < length; i++)
        buffer[i] = dsp_dcBlockFilter(dcb, buffer[i]);
}

/**
 * Data structure holding the internal state of a decimation block.
 */
struct decimatorState {
    int32_t accumulator;
    uint32_t count;
};

/**
 * @brief Run a single step of the oversampling decimation filter.
 *
 * Accumulates input samples and returns their truncated integer average
 * once the oversampling factor number of samples have been collected.
 *
 * @param state: pointer to the decimator state.
 * @param sample: pointer to the audio sample. Contains the value of the
 * decimated output when the function returns true.
 * @param ratio: sample decimation ratio.
 * @return true when a decimated sample is ready, false otherwise.
 */
bool dsp_decimator(struct decimatorState *state, int16_t *sample,
                   uint16_t ratio);

/**
 * Data structure holding the state of a second order (biquad) IIR filter, in
 * transposed direct form II, operating on normalised float samples.
 */
struct biquad {
    float b0, b1, b2, a1, a2;
    float z1, z2;
};

/**
 * Configure a biquad as a second order Butterworth high-pass filter and clear
 * its state.
 *
 * @param bq: filter.
 * @param cutoff: -3 dB frequency, in Hz.
 * @param sampleRate: sampling rate, in Hz.
 */
void dsp_highPassInit(struct biquad *bq, float cutoff, float sampleRate);

/**
 * Run one sample through a biquad.
 *
 * @param bq: filter.
 * @param x: input sample.
 * @return the filtered sample.
 */
static inline float dsp_biquad(struct biquad *bq, const float x)
{
    float y = (bq->b0 * x) + bq->z1;
    bq->z1 = (bq->b1 * x) - (bq->a1 * y) + bq->z2;
    bq->z2 = (bq->b2 * x) - (bq->a2 * y);
    return y;
}

/**
 * Parameters of the automatic gain control, all levels in dBFS of the
 * normalised (+-1.0) signal.
 */
struct agcParams {
    float targetDb;       ///< Speech level the AGC aims at (block RMS)
    float minGainDb;      ///< Lowest gain the AGC applies
    float maxGainDb;      ///< Highest gain the AGC applies
    float attackDbPerBlk; ///< Largest gain decrease per block
    float releaseDbPerBlk;///< Largest gain increase per block
    float fastDownDb;     ///< Pull the gain down at once when a block is this far over target (0 = off)
    float gateAboveFloor; ///< Gate opens this many dB above the noise floor
    float gateMinDb;      ///< Gate never opens below this level
    float floorRiseDbPerBlk; ///< Noise floor tracker rise rate
    float kneeDb;         ///< Limiter knee
    float ceilingDb;      ///< Limiter ceiling, never exceeded
};

/**
 * Data structure holding the state of the automatic gain control.
 */
struct agcState {
    struct agcParams p;
    float gainDb;       ///< Gain applied at the end of the last block
    float levelDb;      ///< Smoothed speech level estimate
    float floorDb;      ///< Noise floor estimate
    bool gateOpen;      ///< Gate state of the last block
    uint16_t acquire;   ///< Speech blocks left in the fast acquisition phase
};

/**
 * Default AGC parameters for microphone audio feeding codec2 at 8 kHz in
 * 160 sample (20 ms) blocks.
 */
void dsp_agcDefaults(struct agcParams *params);

/**
 * Initialise the AGC state with the given parameters, starting at 0 dB gain.
 * For the first second of speech after initialisation the gain may rise ten
 * times faster than the release rate, so that a first transmission settles
 * quickly; afterwards the state is meant to be kept across transmissions.
 */
void dsp_agcInit(struct agcState *agc, const struct agcParams *params);

/**
 * Apply automatic gain control and peak limiting to a block of normalised
 * samples, in place.
 *
 * The gain for the block is decided from the block's own level, then ramped
 * linearly from the previous block's gain across the block. Blocks quieter
 * than the gate (an adaptive threshold above the tracked noise floor) hold the
 * gain, so pauses do not pump the background noise up. A soft limiter follows
 * the gain, keeping every sample below the ceiling.
 *
 * @param agc: AGC state.
 * @param block: normalised samples, processed in place.
 * @param len: number of samples.
 */
void dsp_agcProcess(struct agcState *agc, float *block, size_t len);

/**
 * Soft limiter: linear below the knee, then a tanh curve approaching the
 * ceiling. Both arguments are linear amplitudes.
 */
float dsp_softLimit(float x, float knee, float ceiling);

#ifdef __cplusplus
}
#endif // __cplusplus

#endif /* DSP_H */
