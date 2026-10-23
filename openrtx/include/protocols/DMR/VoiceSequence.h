/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_VOICESEQUENCE_H
#define DMR_VOICESEQUENCE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The bursts of a DMR voice transmission, independently of the baseband
 * sending them: the call opens with voice link control headers, goes on with
 * superframes of voice frames A to F, and once asked to stop sends a
 * terminator after the next frame F, then ends. In direct mode one slot in two
 * is sent, the other left idle; through a repeater the slots sent are those of
 * the channel's timeslot.
 */

/** Frames in a voice superframe, A to F. */
#define DMR_SUPERFRAME_FRAMES 6

/**
 * What to do in a slot.
 */
enum dmrTxSlot {
    DMR_TX_IDLE,       ///< The idle slot: send nothing.
    DMR_TX_HEADER,     ///< Send a voice link control header.
    DMR_TX_VOICE,      ///< Send a voice frame.
    DMR_TX_TERMINATOR, ///< Send the terminator.
    DMR_TX_END         ///< The transmission is over.
};

/**
 * A slot of the transmission.
 */
struct dmrTxBurst {
    enum dmrTxSlot type; ///< What to do.
    uint8_t frame;       ///< Voice frame, 0 to 5 for A to F.
    bool lastHeader;     ///< The last of the headers.
};

/**
 * State of a transmission.
 */
struct dmrTxSequence {
    uint8_t headersLeft; ///< Voice headers still to send.
    uint8_t frame;       ///< Next voice frame, 0 to 5.
    bool atBoundary;     ///< The last voice frame sent was F.
    bool terminated;     ///< The terminator has been sent.
    bool idleNext;       ///< The next slot is the idle one.
};

/**
 * Start a transmission, its first slot sent.
 *
 * @param seq: transmission state.
 * @param headers: number of voice headers to open with.
 */
void dmrTx_start(struct dmrTxSequence *seq, const uint8_t headers);

/**
 * Get what to do in the next slot.
 *
 * @param seq: transmission state.
 * @param stopRequested: the transmission has been asked to stop.
 * @return the slot's burst.
 */
struct dmrTxBurst dmrTx_nextSlot(struct dmrTxSequence *seq,
                                 const bool stopRequested);

/**
 * Get the burst to send in the next slot of the transmission's own, when
 * something else, such as a repeater's timeslot, decides which slots those
 * are.
 *
 * @param seq: transmission state.
 * @param stopRequested: the transmission has been asked to stop.
 * @return the burst, never DMR_TX_IDLE.
 */
struct dmrTxBurst dmrTx_nextBurst(struct dmrTxSequence *seq,
                                  const bool stopRequested);

#ifdef __cplusplus
}
#endif

#endif /* DMR_VOICESEQUENCE_H */
