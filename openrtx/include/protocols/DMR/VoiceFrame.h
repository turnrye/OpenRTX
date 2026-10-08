/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_VOICEFRAME_H
#define DMR_VOICEFRAME_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Length of a DMR AMBE voice frame, in bytes: the 72 bits of one 20ms frame as
 * sent on air, three of them making up the payload of a voice burst.
 */
#define DMR_VOICE_FRAME_LENGTH 9

/**
 * The AMBE frame radios send when there is no voice: the fixed, public
 * pattern every receiver decodes as silence.
 */
extern const uint8_t DMR_VOICE_SILENCE_FRAME[DMR_VOICE_FRAME_LENGTH];

/**
 * Estimate the bit errors in the first codeword of a voice frame, the one
 * protected by a Golay(23,12) code. A frame sent as voice decodes almost error
 * free; noise, such as what the receiver picks up on the empty timeslot in
 * direct mode, is far from any codeword.
 *
 * The estimate is exact for no error and for a single one. Two or more read
 * as at least two, up to 4, which stands for an estimate that gave up.
 *
 * @param frame: voice frame, DMR_VOICE_FRAME_LENGTH bytes as sent on air.
 * @return estimated bit errors, from 0 to 4.
 */
uint8_t dmr_voiceFrameErrors(const uint8_t *frame);

/**
 * Tell a voice frame from noise: at most one bit error in its first codeword,
 * and not the all-ones pattern the HR_C6000 hands over when it has nothing,
 * whose first codeword happens to be a valid one.
 *
 * @param frame: voice frame, DMR_VOICE_FRAME_LENGTH bytes as sent on air.
 * @return true if the frame carries voice.
 */
bool dmr_isVoiceFrame(const uint8_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* DMR_VOICEFRAME_H */
