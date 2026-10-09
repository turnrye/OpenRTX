/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include "protocols/DMR/LinkControl.h"

TEST_CASE("DMR group call link control", "[dmr][lc]")
{
    uint8_t lc[DMR_LC_LENGTH];
    memset(lc, 0xAA, sizeof(lc));

    dmr_buildVoiceLc(lc, true, 91, 3101003);

    const uint8_t expected[DMR_LC_LENGTH] = {
        DMR_FLCO_GROUP, 0x00, 0x00, // Call type, feature set, options
        0x00,           0x00, 0x5B, // Talkgroup 91
        0x2F,           0x51, 0x4B, // Source 3101003
        0x00,           0x00, 0x00  // Error correction, from the chip
    };

    REQUIRE(memcmp(lc, expected, sizeof(lc)) == 0);
}

TEST_CASE("DMR private call link control", "[dmr][lc]")
{
    uint8_t lc[DMR_LC_LENGTH];
    dmr_buildVoiceLc(lc, false, 2357322, 3101003);

    REQUIRE(lc[0] == DMR_FLCO_PRIVATE);
    REQUIRE(lc[3] == 0x23);
    REQUIRE(lc[4] == 0xF8);
    REQUIRE(lc[5] == 0x4A);
}

TEST_CASE("DMR link control addresses are 24 bits", "[dmr][lc]")
{
    uint8_t lc[DMR_LC_LENGTH];

    // The All Call address, the largest there is
    dmr_buildVoiceLc(lc, true, 0xFFFFFF, 0xFFFFFF);
    for (int i = 3; i < 9; i++)
        REQUIRE(lc[i] == 0xFF);

    // Anything above 24 bits does not fit and is cut down
    dmr_buildVoiceLc(lc, true, 0x1000005, 0x2000006);
    REQUIRE(lc[3] == 0x00);
    REQUIRE(lc[5] == 0x05);
    REQUIRE(lc[6] == 0x00);
    REQUIRE(lc[8] == 0x06);
}

TEST_CASE("DMR link control parses back what was built", "[dmr][lc]")
{
    uint8_t lc[DMR_LC_LENGTH];
    struct dmrVoiceCall call;

    dmr_buildVoiceLc(lc, true, 91, 3101003);
    REQUIRE(dmr_parseVoiceLc(lc, &call));
    REQUIRE(call.groupCall);
    REQUIRE(call.dstId == 91);
    REQUIRE(call.srcId == 3101003);
    REQUIRE_FALSE(call.encrypted);

    dmr_buildVoiceLc(lc, false, 2357322, 0xFFFFFF);
    REQUIRE(dmr_parseVoiceLc(lc, &call));
    REQUIRE_FALSE(call.groupCall);
    REQUIRE(call.dstId == 2357322);
    REQUIRE(call.srcId == 0xFFFFFF);
}

TEST_CASE("DMR link control parse reads privacy and refuses other opcodes",
          "[dmr][lc]")
{
    uint8_t lc[DMR_LC_LENGTH];
    struct dmrVoiceCall call;

    dmr_buildVoiceLc(lc, true, 9, 1234);
    lc[2] = 0x40; // Service options: privacy
    REQUIRE(dmr_parseVoiceLc(lc, &call));
    REQUIRE(call.encrypted);

    // The protect flag and reserved bit do not change the opcode
    lc[0] = 0x80 | DMR_FLCO_GROUP;
    REQUIRE(dmr_parseVoiceLc(lc, &call));

    // Talker alias header, GPS info: not a voice call
    for (uint8_t flco : { 0x04, 0x08, 0x05 }) {
        lc[0] = flco;
        call.dstId = 77;
        REQUIRE_FALSE(dmr_parseVoiceLc(lc, &call));
        REQUIRE(call.dstId == 77); // Untouched
    }
}
