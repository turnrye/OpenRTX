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
 * DMR voice codec (AMBE+2 half rate). The vocoder itself is a proprietary blob
 * that is not part of OpenRTX: it is installed from the user's own donor
 * firmware at a fixed flash address by the separate DMR installer. This layer
 * only calls into it, mirroring how the donor firmware drives it; without the
 * blob installed the codec is unavailable and DMR degrades to header-only RX.
 *
 * A voice frame is one 20 ms AMBE frame: 160 PCM samples at 8 kHz in, 9 bytes
 * out (three of these make up the 27-byte payload of a DMR voice burst).
 */
#define DMR_CODEC_PCM_SAMPLES 160
#define DMR_CODEC_FRAME_BYTES 9

/**
 * @return true if the donor vocoder is installed at the expected flash address.
 */
bool dmr_codec_available(void);

/**
 * Bring the vocoder up: run the donor's initialisation sequence over the codec
 * state buffers. Must be called before encode/decode.
 *
 * @return true on success, false if the vocoder blob is not installed.
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
