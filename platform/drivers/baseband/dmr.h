/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_H
#define DMR_H

#include <stdint.h>
#include <stdbool.h>
#include "protocols/DMR/Datatypes.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The DMR modem contract: what every platform's DMR baseband driver
 * implements, in the protocol layer's own vocabulary. A modem takes in and
 * gives out bursts - that is the boundary the hardware draws - and knows
 * nothing about calls: the call state lives above, in protocols/DMR, driven
 * by the operating mode.
 */

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
 * Poll the modem for reception. This is the periodic housekeeping step - it
 * has to be called from a thread, faster than the 30ms slot time, as it
 * accesses the baseband over SPI - and it reports the burst of the most
 * recent frame event. Every frame event yields a burst, including corrupted
 * and unclassified ones: the call state's liveness and terminator rules
 * feed on events which fail their checks.
 *
 * @param burst: destination for the received burst.
 * @return true if a frame event occurred and the burst describes it.
 */
bool dmr_rxPoll(struct dmrBurst *burst);

/**
 * Key a transmission carrying the given link control. The transmission runs
 * from the timeslot interrupt until dmr_stopTx is called; this only sets it
 * up. What the call is - its type, its addresses - is entirely in the link
 * control the caller built: the modem carries content, it does not compose
 * it.
 *
 * @param lc: link control to transmit, DMR_LC_LENGTH information octets.
 */
void dmr_startTx(const uint8_t *lc);

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
 * Perform a configuration register access asked for over the debug tap. The
 * access is deferred to the next receive poll, where the bus is already fenced
 * off from the interrupt handlers, and its result is logged there.
 *
 * This is a bench instrument for finding out what the chip exposes, and it will
 * happily write a register which stops the radio working; the way back is a
 * mode change, which rewrites the configuration.
 *
 * @param op: 'r' to read one register, 'w' to write one, 'd' to dump a range.
 * @param addr: register address, or the first of the range.
 * @param value: value to write, or the last register of the range.
 */
void dmr_debugRegisterOp(char op, uint8_t addr, uint8_t value);

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
