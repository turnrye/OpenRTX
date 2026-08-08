/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_TXSCHEDULE_H
#define DMR_TXSCHEDULE_H

#include <stdint.h>
#include <stdbool.h>
#include "protocols/DMR/Datatypes.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The direct mode transmit call procedure: what burst goes out on which
 * slot. A call opens with three voice link control headers on alternating
 * slots, carries voice superframes with sequence numbers cycling 0 to 5,
 * and closes on a terminator at a superframe boundary - that is air
 * interface procedure, identical whatever chip transmits it, so it lives
 * here rather than in a driver.
 *
 * The step function is pure bookkeeping - no locks, no allocation, no
 * hardware - which is what makes it callable straight from a modem's slot
 * interrupt, where the next burst must be decided within a slot time. The
 * modem translates each returned slot into its own registers and owns the
 * timing quirks of doing so; it contributes the voice payload itself, as
 * payloads come from the audio pipeline the protocol layer never touches.
 */

/** What the transmit procedure wants in the upcoming slot. */
enum dmrTxSlot {
    DMR_TX_SLOT_NONE = 0,     ///< No transmission is in progress.
    DMR_TX_SLOT_IDLE,         ///< Leave the slot idle.
    DMR_TX_SLOT_FIRST_HEADER, ///< Voice header; the link control is handed
                              ///  to the modem with this first one.
    DMR_TX_SLOT_HEADER,       ///< Voice header repeating the link control.
    DMR_TX_SLOT_VOICE,        ///< Voice burst; sequence in the out param.
    DMR_TX_SLOT_TERMINATOR,   ///< Terminator carrying the link control.
    DMR_TX_SLOT_FINISH,       ///< The terminator has played out: return
                              ///  the modem to reception.
};

/**
 * State of the transmit procedure. The stop request is written from a
 * thread while the modem's interrupt advances the machine, hence volatile;
 * everything else is touched only with the machine stopped or from the
 * advancing context.
 */
struct dmrTxSchedule {
    volatile uint8_t state;      ///< Position in the procedure.
    volatile bool stopRequested; ///< Unkey asked, waiting for a boundary.
    uint8_t sequence;            ///< Voice frame sequence, 0 to 5.
};

/**
 * Reset the procedure to not transmitting.
 *
 * @param ts: procedure state to initialise.
 */
void dmr_txSchedule_init(struct dmrTxSchedule *ts);

/**
 * Start a transmission: the next advance returns the first header. Only to
 * be called with the machine stopped and the modem not yet raising slot
 * interrupts for it.
 *
 * @param ts: procedure state to start.
 */
void dmr_txSchedule_start(struct dmrTxSchedule *ts);

/**
 * Ask the transmission to end. The terminator goes out at the next
 * superframe boundary, never mid-superframe.
 *
 * @param ts: procedure state of the running transmission.
 */
void dmr_txSchedule_stop(struct dmrTxSchedule *ts);

/**
 * Whether a transmission is still in progress: true from start until the
 * finish slot has been returned.
 *
 * @param ts: procedure state to query.
 * @return true while the procedure runs.
 */
bool dmr_txSchedule_active(const struct dmrTxSchedule *ts);

/**
 * Advance the procedure by one slot and say what belongs in it. Called once
 * per timeslot interrupt while transmitting.
 *
 * @param ts: procedure state to advance.
 * @param sequence: destination for the voice frame sequence number, written
 * for DMR_TX_SLOT_VOICE only.
 * @return what the modem is to do with the upcoming slot.
 */
enum dmrTxSlot dmr_txSchedule_advance(struct dmrTxSchedule *ts,
                                      uint8_t *sequence);

#ifdef __cplusplus
}
#endif

#endif /* DMR_TXSCHEDULE_H */
