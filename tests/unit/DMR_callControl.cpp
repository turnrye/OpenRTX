/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include "protocols/DMR/CallControl.h"
#include "protocols/DMR/LinkControl.h"

struct Lc {
    uint8_t b[DMR_LC_LENGTH];
    Lc(bool group, uint32_t dst, uint32_t src)
    {
        dmr_buildVoiceLc(b, group, dst, src);
    }
};

TEST_CASE("DMR call starts from its link control and ends on its terminator",
          "[dmr][call]")
{
    struct dmrRxCall rx;
    dmrCall_reset(&rx);
    REQUIRE_FALSE(rx.active);

    Lc lc(true, 9, 3101003);
    REQUIRE(dmrCall_takeLc(&rx, lc.b, 1, 1000) == DMR_CALL_STARTED);
    REQUIRE(rx.active);
    REQUIRE(rx.call.dstId == 9);
    REQUIRE(rx.colorCode == 1);

    // The same call again, from the embedded signalling: no new call
    REQUIRE(dmrCall_takeLc(&rx, lc.b, 1, 1360) == DMR_CALL_SAME);

    REQUIRE(dmrCall_takeTerminator(&rx, lc.b) == DMR_CALL_ENDED);
    REQUIRE_FALSE(rx.active);

    // A late terminator for a call that is over changes nothing
    REQUIRE(dmrCall_takeTerminator(&rx, lc.b) == DMR_CALL_IGNORED);
}

TEST_CASE("DMR another station's link control starts a new call", "[dmr][call]")
{
    struct dmrRxCall rx;
    dmrCall_reset(&rx);

    Lc first(true, 9, 3101003);
    Lc other(true, 9, 2357322);
    Lc privateCall(false, 9, 3101003);

    dmrCall_takeLc(&rx, first.b, 1, 0);
    REQUIRE(dmrCall_takeLc(&rx, other.b, 1, 60) == DMR_CALL_STARTED);
    REQUIRE(rx.call.srcId == 2357322);

    // Same addresses, but a private call: another call
    REQUIRE(dmrCall_takeLc(&rx, privateCall.b, 1, 120) == DMR_CALL_STARTED);
    REQUIRE_FALSE(rx.call.groupCall);
}

TEST_CASE("DMR terminator of another call is ignored", "[dmr][call]")
{
    struct dmrRxCall rx;
    dmrCall_reset(&rx);

    Lc mine(true, 9, 3101003);
    Lc theirs(true, 91, 2357322);
    dmrCall_takeLc(&rx, mine.b, 1, 0);
    REQUIRE(dmrCall_takeTerminator(&rx, theirs.b) == DMR_CALL_IGNORED);
    REQUIRE(rx.active);
}

TEST_CASE("DMR link control that is not a voice call is ignored", "[dmr][call]")
{
    struct dmrRxCall rx;
    dmrCall_reset(&rx);

    Lc lc(true, 9, 3101003);
    lc.b[0] = 0x04; // Talker alias header
    REQUIRE(dmrCall_takeLc(&rx, lc.b, 1, 0) == DMR_CALL_IGNORED);
    REQUIRE_FALSE(rx.active);
    REQUIRE(dmrCall_takeTerminator(&rx, lc.b) == DMR_CALL_IGNORED);
}

TEST_CASE("DMR voice holds a call, and a call without bursts expires",
          "[dmr][call]")
{
    struct dmrRxCall rx;
    dmrCall_reset(&rx);

    // No call: voice is not taken, nothing expires
    REQUIRE_FALSE(dmrCall_takeVoice(&rx, 0));
    REQUIRE_FALSE(dmrCall_expired(&rx, 10000, 500));

    Lc lc(true, 9, 3101003);
    dmrCall_takeLc(&rx, lc.b, 1, 0);
    for (long long t = 60; t <= 3000; t += 60) {
        REQUIRE(dmrCall_takeVoice(&rx, t));
        REQUIRE_FALSE(dmrCall_expired(&rx, t + 30, 500));
    }

    REQUIRE_FALSE(dmrCall_expired(&rx, 3500, 500));
    REQUIRE(dmrCall_expired(&rx, 3501, 500));
}

TEST_CASE("DMR colour code is carried by every burst but voice frame A",
          "[dmr][call]")
{
    REQUIRE(dmr_burstHasColorCode(DMR_BURST_DATA));
    REQUIRE(dmr_burstHasColorCode(DMR_BURST_VOICE));
    REQUIRE_FALSE(dmr_burstHasColorCode(DMR_BURST_VOICE_SYNC));
}
