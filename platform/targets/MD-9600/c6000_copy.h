/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef C6000_COPY_H
#define C6000_COPY_H

#include <stdint.h>
#include "interfaces/audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Audio input driver for the receive baseband from the HR_C6000 FM receive
 * copy, 24 kHz, circular double buffer mode only.
 */
extern const struct audioDriver c6000_copy_audio_driver;

/**
 * Counters: copy halves read, halves picked by order alone, samples dropped.
 */
void c6000copy_getStats(uint32_t *halves, uint32_t *fixes, uint32_t *drops);

#ifdef __cplusplus
}
#endif

#endif /* C6000_COPY_H */
