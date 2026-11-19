/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef I2S_PARROT_H
#define I2S_PARROT_H

#include <stdbool.h>
#include <stdint.h>
#include "drivers/audio/stm32_i2s.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bench test of the HR_C6000 microphone PCM over I2S3: record three seconds
 * from SOURCE_MIC, play them back to the speaker, repeat. Never transmits.
 */

enum ParrotPhase
{
    PARROT_IDLE = 0,
    PARROT_REC,
    PARROT_PLAY,
    PARROT_ERROR
};

struct parrotStatus
{
    uint8_t  phase;     ///< ParrotPhase
    bool     right;     ///< Channel the stream carries
    uint8_t  gain;      ///< Playback gain applied to the last take
    int16_t  error;     ///< Last stream or path error, 0 if none
    uint32_t takes;     ///< Takes recorded
    struct i2sStats stats;  ///< Levels of the last take
    struct i2sDiag  diag;   ///< Live diagnostics, once a second
    struct i2sStats live;   ///< Levels of the take in progress
    uint8_t  exp;           ///< HR_C6000 set-up of the take, see i2s_parrot.c
    uint8_t  regs[6];       ///< HR_C6000 0x10, 0x22, 0xE0, 0x36, 0xE2, 0x06
};

/**
 * Start the parrot thread.
 */
void i2sParrot_start();

/**
 * Get the parrot state for display.
 *
 * @param status: destination.
 */
void i2sParrot_getStatus(struct parrotStatus *status);

#ifdef __cplusplus
}
#endif

#endif /* I2S_PARROT_H */
