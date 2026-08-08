/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/DMR/TxSchedule.h"

/*
 * Positions in the transmit procedure. The call opens with three voice link
 * control headers on alternating slots, then voice superframes, and closes
 * on a terminator once the transmission has been asked to stop at a
 * superframe boundary. The gap steps leave the other slot idle, as a direct
 * mode station transmits on a single slot.
 */
enum txState {
    TX_IDLE = 0, ///< Not transmitting.
    TX_START_1,  ///< First header burst.
    TX_START_2,  ///< Idle slot.
    TX_START_3,  ///< Second header burst.
    TX_START_4,  ///< Idle slot.
    TX_START_5,  ///< Third header burst.
    TX_GAP,      ///< Idle slot between voice frames.
    TX_VOICE,    ///< Voice frame.
    TX_END_1,    ///< Terminator burst.
    TX_END_2,    ///< Terminator going out on air.
    TX_END_3,    ///< Return to reception.
};

void dmr_txSchedule_init(struct dmrTxSchedule *ts)
{
    ts->state = TX_IDLE;
    ts->stopRequested = false;
    ts->sequence = 0;
}

void dmr_txSchedule_start(struct dmrTxSchedule *ts)
{
    ts->sequence = 0;
    ts->stopRequested = false;
    ts->state = TX_START_1;
}

void dmr_txSchedule_stop(struct dmrTxSchedule *ts)
{
    ts->stopRequested = true;
}

bool dmr_txSchedule_active(const struct dmrTxSchedule *ts)
{
    return ts->state != TX_IDLE;
}

enum dmrTxSlot dmr_txSchedule_advance(struct dmrTxSchedule *ts,
                                      uint8_t *sequence)
{
    switch (ts->state) {
        case TX_START_1:
            ts->state = TX_START_2;
            return DMR_TX_SLOT_FIRST_HEADER;

        case TX_START_2:
            ts->state = TX_START_3;
            return DMR_TX_SLOT_IDLE;

        case TX_START_3:
            ts->state = TX_START_4;
            return DMR_TX_SLOT_HEADER;

        case TX_START_4:
            ts->state = TX_START_5;
            return DMR_TX_SLOT_IDLE;

        case TX_START_5:
            ts->state = TX_GAP;
            return DMR_TX_SLOT_HEADER;

        /*
         * The idle slot preceding each voice frame. Leaving the transmission
         * at a superframe boundary, when the next frame would be the first
         * of a superframe, keeps the terminator on the correct slot.
         */
        case TX_GAP:
            if (ts->stopRequested && (ts->sequence == 0))
                ts->state = TX_END_1;
            else
                ts->state = TX_VOICE;
            return DMR_TX_SLOT_IDLE;

        case TX_VOICE:
            *sequence = ts->sequence;
            ts->sequence = (ts->sequence + 1) % DMR_SUPERFRAME_FRAMES;
            ts->state = TX_GAP;
            return DMR_TX_SLOT_VOICE;

        /*
         * The terminator carries the link control again so a late listener
         * can still identify the call as it ends. The slot after it exists
         * so the burst can play out on air untouched; only then is the modem
         * told to go back to reception.
         */
        case TX_END_1:
            ts->state = TX_END_2;
            return DMR_TX_SLOT_TERMINATOR;

        case TX_END_2:
            ts->state = TX_END_3;
            return DMR_TX_SLOT_IDLE;

        case TX_END_3:
            ts->state = TX_IDLE;
            return DMR_TX_SLOT_FINISH;

        default:
            return DMR_TX_SLOT_NONE;
    }
}
