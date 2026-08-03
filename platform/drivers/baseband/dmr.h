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

/**
 * Key a direct mode group or private call and start sending the constant
 * silence payload. The transmission runs from the timeslot interrupt until
 * dmr_stopTx is called; this only sets it up.
 *
 * @param groupCall: true for a group call, false for a private one.
 * @param dstId: destination address, a talkgroup for a group call.
 * @param srcId: source address, that is the radio's own DMR ID.
 */
void dmr_startTx(const bool groupCall, const uint32_t dstId,
                 const uint32_t srcId);

/**
 * Ask the current transmission to end. The terminator is sent at the next
 * superframe boundary, after which dmr_txActive returns false.
 */
void dmr_stopTx();

/**
 * Periodic transmit-side housekeeping, called from a thread while a
 * transmission is in progress. It keeps the interrupt watchdog fed, which the
 * receive poll normally does but cannot while transmitting.
 */
void dmr_txTask();

/**
 * Check whether a transmission is still in progress.
 *
 * @return true until the terminator has been sent and the chip handed back to
 * reception.
 */
bool dmr_txActive();

/**
 * Advance the transmit slot state machine by one slot. Called from the
 * timeslot interrupt handler while a transmission is active, it must not be
 * used from a thread as it drives the chip without taking the SPI mutex.
 */
void dmr_txTimeslotIsr();

#ifdef __cplusplus
}
#endif

#endif /* DMR_H */
