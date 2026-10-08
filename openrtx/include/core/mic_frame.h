/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MIC_FRAME_H
#define MIC_FRAME_H

#include "core/dsp.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Microphone frames for the voice encoders. Every encoder takes the same 20ms
 * frames of 160 samples at 8kHz, conditioned the same way from the oversampled
 * ADC stream: decimated, DC blocked and amplified. On top of that a gate tells
 * frames with voice in them from the ones carrying only the room, so that an
 * encoder can send its silence frame instead of encoding the noise, as other
 * radios do; which frame that is belongs to each encoder.
 *
 * The gate works on the energy of a frame, with two thresholds so that it does
 * not chatter, and holds open for a while after the voice drops below them so
 * that the tail of a word is not cut. A radio sets the thresholds for its
 * microphone with CONFIG_MIC_GATE_OPEN and CONFIG_MIC_GATE_CLOSE, as RMS
 * sample values after the gain; without them the gate stays open.
 */

/** Samples in a frame: 20ms at 8kHz. */
#define MIC_FRAME_SAMPLES 160

/** Frames the gate holds open after the voice drops below the thresholds. */
#define MIC_GATE_HOLD_FRAMES 10

struct micFrameState {
    struct dcBlock dcBlock;
    struct decimatorState decimator;
    uint16_t openLevel;  ///< RMS opening the gate, 0 for always open.
    uint16_t closeLevel; ///< RMS below which it closes.
    uint16_t hold;       ///< Frames left before closing.
    bool open;           ///< Gate state.
};

/**
 * Set a state up for a new capture, with the thresholds of the radio.
 *
 * @param state: state to initialise.
 */
void micFrame_init(struct micFrameState *state);

/**
 * Condition one block of the oversampled ADC stream into a frame, in place:
 * decimate by CONFIG_MIC_OVERSAMPLE, block the DC and apply CONFIG_MIC_GAIN.
 *
 * @param state: capture state.
 * @param buf: samples in, frame out.
 * @param len: number of samples in.
 * @return number of samples in the frame.
 */
size_t micFrame_condition(struct micFrameState *state, int16_t *buf,
                          size_t len);

/**
 * Run the gate over a frame.
 *
 * @param state: capture state.
 * @param frame: conditioned frame.
 * @param len: samples in the frame.
 * @return true if the frame carries voice and is to be encoded, false if the
 * encoder's silence frame goes out in its place.
 */
bool micFrame_hasVoice(struct micFrameState *state, const int16_t *frame,
                       size_t len);

/**
 * RMS of a frame, the measure the gate thresholds are set in.
 *
 * @param frame: samples.
 * @param len: number of samples.
 * @return RMS sample value.
 */
uint16_t micFrame_rms(const int16_t *frame, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* MIC_FRAME_H */
