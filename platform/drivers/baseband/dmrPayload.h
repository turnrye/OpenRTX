/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_PAYLOAD_H
#define DMR_PAYLOAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Board driver for the HR_C6000's second SPI bus, the one carrying the voice
 * payload of DMR bursts, separate from the control bus. The chip exposes the
 * payload of the burst it just decoded, and takes the payload of the burst it
 * transmits next, as three 9-byte AMBE frames.
 */

/** Length of a burst's voice payload, in bytes. */
#define DMR_PAYLOAD_LENGTH 27

/**
 * Bring up the payload bus.
 */
void dmrPayload_init();

/**
 * Shut the payload bus down and park its lines.
 */
void dmrPayload_terminate();

/**
 * Read the voice payload of the burst the chip has just decoded.
 *
 * @param payload: destination buffer, DMR_PAYLOAD_LENGTH bytes long.
 */
void dmrPayload_read(uint8_t *payload);

/**
 * Load the voice payload the chip transmits in the upcoming slot. Safe to call
 * from the timeslot interrupt.
 *
 * @param payload: source buffer, DMR_PAYLOAD_LENGTH bytes long.
 */
void dmrPayload_write(const uint8_t *payload);

#ifdef __cplusplus
}
#endif

#endif /* DMR_PAYLOAD_H */
