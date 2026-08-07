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
 * Colour code standing for "not known". Direct mode carries the colour code
 * only in signalling the chip consumes without exposing, so a received call
 * reports this; a real value needs the CACH, that is repeater mode.
 */
#define DMR_CC_UNKNOWN  0xFF

/**
 * Information carried by the link control of the last accepted burst, which is
 * what the user interface shows as "last heard".
 */
struct dmrRxState
{
    uint32_t srcId;         ///< DMR ID of the station being received.
    uint32_t dstId;         ///< Talkgroup or destination the call is sent to.
    uint8_t  colorCode;     ///< Color code received, DMR_CC_UNKNOWN in DMO.
    bool     groupCall;     ///< True for a group call, false for a private one.
    bool     valid;         ///< True once the link control has been confirmed.
};

/**
 * Everything the DMR receiver and transmitter take from the radio settings.
 * Passed as one block rather than through a setter each: the settings are
 * applied together, whenever the rtx configuration changes, and this is where
 * the receive filters which are still to come belong as well.
 */
struct dmrConfig
{
    uint32_t dmrId;         ///< Own DMR ID, zero when unset.
    uint32_t talkgroup;     ///< Talkgroup transmitted to, and accepted on RX.
    uint8_t  colorCode;     ///< Color code, from 0 to 15.
    bool     rxPromiscuous; ///< Accept calls addressed to anyone.
};

/**
 * Bring the baseband up for DMR reception and start handling its interrupts.
 *
 * @param cfg: configuration the receiver has to start with.
 */
void dmr_init(const struct dmrConfig *cfg);

/**
 * Stop the DMR reception.
 */
void dmr_terminate();

/**
 * Apply a new configuration. The parts of it the chip has to be told about are
 * only written when they have actually changed.
 *
 * @param cfg: configuration to apply.
 */
void dmr_setConfig(const struct dmrConfig *cfg);

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
