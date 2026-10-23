/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_CODEC_H
#define DMR_CODEC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMR voice codec (AMBE+2 half rate). The vocoder itself is proprietary code
 * that is not part of OpenRTX: each radio runs it from where it sits in flash,
 * either merged in from a donor firmware by the separate DMR installer or left
 * in place by the radio's own stock firmware. This layer only calls into it,
 * the way the stock firmwares drive it; without it DMR runs without voice.
 *
 * A voice frame is one 20 ms AMBE frame: 160 PCM samples at 8 kHz in, 9 bytes
 * out (three of these make up the 27-byte payload of a DMR voice burst).
 */
#define DMR_CODEC_PCM_SAMPLES 160
#define DMR_CODEC_FRAME_BYTES 9

/**
 * @return true if the vocoder is in flash where this radio runs it from.
 */
bool dmr_codec_available(void);

/**
 * Bring the vocoder up: run its initialisation sequence over the codec state
 * buffers. Must be called before encode/decode.
 *
 * @return true on success, false if the vocoder is not in flash.
 */
bool dmr_codec_init(void);

/**
 * Release the codec. Idempotent.
 */
void dmr_codec_terminate(void);

/**
 * Encode one 20 ms voice frame.
 *
 * @param frame: destination, DMR_CODEC_FRAME_BYTES (9) bytes of AMBE.
 * @param pcm:   source, DMR_CODEC_PCM_SAMPLES (160) signed 16-bit samples.
 */
void dmr_codec_encode(uint8_t frame[DMR_CODEC_FRAME_BYTES],
                      const int16_t pcm[DMR_CODEC_PCM_SAMPLES]);

/**
 * Decode one 20 ms voice frame.
 *
 * @param pcm:   destination, DMR_CODEC_PCM_SAMPLES (160) signed 16-bit samples.
 * @param frame: source, DMR_CODEC_FRAME_BYTES (9) bytes of AMBE.
 */
void dmr_codec_decode(int16_t pcm[DMR_CODEC_PCM_SAMPLES],
                      const uint8_t frame[DMR_CODEC_FRAME_BYTES]);

/**
 * Run the vocoder self-test and report over the debug tap: check the blob is
 * present, initialise, encode a baked test tone (streaming the AMBE frames as
 * "$CE" records for an off-radio decoder to verify), and round-trip one frame.
 * This is the golden-vector bring-up used before wiring the codec to the voice
 * path. Triggered by the 'c' command on the tap.
 */
void dmr_codec_selfTest(void);

#ifdef __cplusplus
}
#endif

#endif /* DMR_CODEC_H */
