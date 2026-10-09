/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/DMR/VoiceSequence.h"
#include <string.h>

void dmrTx_start(struct dmrTxSequence *seq, const uint8_t headers)
{
    memset(seq, 0, sizeof(*seq));
    seq->headersLeft = headers;
}

struct dmrTxBurst dmrTx_nextSlot(struct dmrTxSequence *seq,
                                 const bool stopRequested)
{
    struct dmrTxBurst burst = { DMR_TX_IDLE, 0, false };

    if (seq->idleNext) {
        seq->idleNext = false;
        return burst;
    }

    seq->idleNext = true;

    if (seq->terminated) {
        burst.type = DMR_TX_END;
        return burst;
    }

    if (seq->headersLeft > 0) {
        seq->headersLeft -= 1;
        burst.type = DMR_TX_HEADER;
        burst.lastHeader = (seq->headersLeft == 0);
        return burst;
    }

    if (seq->atBoundary && stopRequested) {
        seq->terminated = true;
        burst.type = DMR_TX_TERMINATOR;
        return burst;
    }

    burst.type = DMR_TX_VOICE;
    burst.frame = seq->frame;
    seq->atBoundary = (seq->frame == (DMR_SUPERFRAME_FRAMES - 1));
    seq->frame = (seq->frame + 1) % DMR_SUPERFRAME_FRAMES;

    return burst;
}
