/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_H
#define DMR_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Information carried by the link control of the last accepted burst, which is
 * what the user interface shows as "last heard".
 */
struct dmrRxState
{
    uint32_t srcId;         ///< DMR ID of the station being received.
    uint32_t dstId;         ///< Talkgroup or destination the call is sent to.
    uint8_t  colorCode;     ///< Color code the burst was received with.
    bool     groupCall;     ///< True for a group call, false for a private one.
    bool     valid;         ///< True once the link control has been confirmed.
};

/**
 * Bring the baseband up for DMR reception and start handling its interrupts.
 *
 * @param colorCode: color code the receiver has to accept.
 */
void dmr_init(const uint8_t colorCode);

/**
 * Stop the DMR reception.
 */
void dmr_terminate();

/**
 * Change the color code the receiver accepts.
 *
 * @param colorCode: color code the receiver has to accept.
 */
void dmr_setColorCode(const uint8_t colorCode);

/**
 * Periodic update step, has to be called from a thread as it accesses the
 * baseband over SPI.
 *
 * @param rx: destination for the current receive state.
 */
void dmr_task(struct dmrRxState *rx);

#ifdef __cplusplus
}
#endif

#endif /* DMR_H */
