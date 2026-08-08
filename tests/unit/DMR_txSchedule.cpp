/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>
#include "protocols/DMR/TxSchedule.h"

/*
 * The transmit procedure earned its shape on the bench: a terminator sent
 * off the superframe boundary lands on the wrong slot, and a teardown
 * touching the chip while the terminator plays out cut the burst short and
 * wedged the chip - the far end never saw the call end. These tests pin the
 * procedure itself; the driver owns the register writes it maps to.
 */

// Advance the procedure once, requiring the given slot.
static void expectSlot(struct dmrTxSchedule *ts, enum dmrTxSlot expected)
{
    uint8_t sequence = 0;
    REQUIRE(dmr_txSchedule_advance(ts, &sequence) == expected);
}

// Run the opening: three headers on alternating slots, ending on the gap
// before the first voice frame.
static void runOpening(struct dmrTxSchedule *ts)
{
    expectSlot(ts, DMR_TX_SLOT_FIRST_HEADER);
    expectSlot(ts, DMR_TX_SLOT_IDLE);
    expectSlot(ts, DMR_TX_SLOT_HEADER);
    expectSlot(ts, DMR_TX_SLOT_IDLE);
    expectSlot(ts, DMR_TX_SLOT_HEADER);
}

TEST_CASE("A transmission opens with three headers on alternating slots",
          "[dmr][txschedule]")
{
    struct dmrTxSchedule ts;
    dmr_txSchedule_init(&ts);

    REQUIRE(dmr_txSchedule_active(&ts) == false);

    dmr_txSchedule_start(&ts);
    REQUIRE(dmr_txSchedule_active(&ts));

    runOpening(&ts);
}

TEST_CASE("Voice frames alternate with idle slots, sequence cycling 0 to 5",
          "[dmr][txschedule]")
{
    struct dmrTxSchedule ts;
    dmr_txSchedule_init(&ts);
    dmr_txSchedule_start(&ts);
    runOpening(&ts);

    std::vector<uint8_t> sequences;
    for (int i = 0; i < 12; i++) {
        uint8_t sequence = 0xFF;
        REQUIRE(dmr_txSchedule_advance(&ts, &sequence) == DMR_TX_SLOT_IDLE);
        REQUIRE(dmr_txSchedule_advance(&ts, &sequence) == DMR_TX_SLOT_VOICE);
        sequences.push_back(sequence);
    }

    for (int i = 0; i < 12; i++)
        REQUIRE(sequences[i] == (i % DMR_SUPERFRAME_FRAMES));
}

TEST_CASE("An unkey request is honoured only at the superframe boundary",
          "[dmr][txschedule]")
{
    struct dmrTxSchedule ts;
    dmr_txSchedule_init(&ts);
    dmr_txSchedule_start(&ts);
    runOpening(&ts);

    // Three voice frames into the superframe...
    uint8_t sequence = 0;
    for (int i = 0; i < 3; i++) {
        expectSlot(&ts, DMR_TX_SLOT_IDLE);
        REQUIRE(dmr_txSchedule_advance(&ts, &sequence) == DMR_TX_SLOT_VOICE);
    }

    // ...the unkey lands mid-superframe...
    dmr_txSchedule_stop(&ts);

    // ...and the remaining three frames still go out before the terminator.
    for (int i = 0; i < 3; i++) {
        expectSlot(&ts, DMR_TX_SLOT_IDLE);
        REQUIRE(dmr_txSchedule_advance(&ts, &sequence) == DMR_TX_SLOT_VOICE);
        REQUIRE(sequence == 3 + i);
    }

    expectSlot(&ts, DMR_TX_SLOT_IDLE);
    expectSlot(&ts, DMR_TX_SLOT_TERMINATOR);
}

TEST_CASE("The terminator gets a slot to play out before the finish",
          "[dmr][txschedule]")
{
    struct dmrTxSchedule ts;
    dmr_txSchedule_init(&ts);
    dmr_txSchedule_start(&ts);
    runOpening(&ts);

    dmr_txSchedule_stop(&ts);

    expectSlot(&ts, DMR_TX_SLOT_IDLE);
    expectSlot(&ts, DMR_TX_SLOT_TERMINATOR);
    REQUIRE(dmr_txSchedule_active(&ts));

    // The slot after the terminator only idles: the burst is on the air and
    // nothing may touch the chip's timing until it has played out.
    expectSlot(&ts, DMR_TX_SLOT_IDLE);
    REQUIRE(dmr_txSchedule_active(&ts));

    expectSlot(&ts, DMR_TX_SLOT_FINISH);
    REQUIRE(dmr_txSchedule_active(&ts) == false);

    expectSlot(&ts, DMR_TX_SLOT_NONE);
}

TEST_CASE("An unkey during the opening sends no voice at all",
          "[dmr][txschedule]")
{
    struct dmrTxSchedule ts;
    dmr_txSchedule_init(&ts);
    dmr_txSchedule_start(&ts);

    dmr_txSchedule_stop(&ts);

    runOpening(&ts);
    expectSlot(&ts, DMR_TX_SLOT_IDLE);
    expectSlot(&ts, DMR_TX_SLOT_TERMINATOR);
    expectSlot(&ts, DMR_TX_SLOT_IDLE);
    expectSlot(&ts, DMR_TX_SLOT_FINISH);
}
