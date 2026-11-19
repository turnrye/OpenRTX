/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef STM32_I2S_H
#define STM32_I2S_H

#include <stdbool.h>
#include <stdint.h>
#include "interfaces/audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Driver for the STM32F4 I2S3 peripheral as an audio input stream device,
 * slave receive: the codec drives CK and WS. Philips standard, 16-bit data in
 * 32-bit channels, 8 kHz only. The stream carries one of the two channels.
 *
 * Pins come from the target's pinmap: I2S3_WS, I2S3_CK and I2S3_SD.
 * DMA1 Stream 0, channel 0.
 */

/**
 * Per channel levels since the last stm32i2s_getStats() with reset set.
 */
struct i2sStats
{
    uint32_t frames;    ///< Frames received
    int16_t  peak[2];   ///< Largest absolute value, left and right
    uint16_t rms[2];    ///< RMS with the mean removed, left and right
    int16_t  mean[2];   ///< Mean value, left and right
    uint32_t noClock;   ///< Stream starts that found no WS clock
};

/**
 * Initialise the I2S3 peripheral, its pins and its DMA stream.
 */
void stm32i2s_init();

/**
 * Shut down the I2S3 peripheral.
 */
void stm32i2s_terminate();

/**
 * Select the channel the stream carries.
 *
 * @param right: true for the right channel, false for the left one.
 */
void stm32i2s_setChannel(const bool right);

/**
 * Get the levels of both channels.
 *
 * @param stats: destination of the levels.
 * @param reset: clear the accumulators after reading them.
 */
void stm32i2s_getStats(struct i2sStats *stats, const bool reset);

extern const struct audioDriver stm32_i2s_audio_driver;

#ifdef __cplusplus
}
#endif

#endif /* STM32_I2S_H */
