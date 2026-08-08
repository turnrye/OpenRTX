/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include "protocols/DMR/LinkControl.h"

TEST_CASE("Group voice link control layout", "[dmr][linkcontrol]")
{
    uint8_t lc[DMR_LC_LENGTH];
    dmr_buildVoiceLc(lc, true, 9, 3101001);

    REQUIRE(lc[0] == DMR_FLCO_GROUP);
    REQUIRE(lc[1] == 0x00); // Feature set ID, standard
    REQUIRE(lc[2] == 0x00); // Service options, none

    // Destination 9, big endian over three octets
    REQUIRE(lc[3] == 0x00);
    REQUIRE(lc[4] == 0x00);
    REQUIRE(lc[5] == 0x09);

    // Source 3101001 = 0x2F5149, big endian over three octets
    REQUIRE(lc[6] == 0x2F);
    REQUIRE(lc[7] == 0x51);
    REQUIRE(lc[8] == 0x49);
}

TEST_CASE("Private call carries the unit to unit opcode", "[dmr][linkcontrol]")
{
    uint8_t lc[DMR_LC_LENGTH];
    dmr_buildVoiceLc(lc, false, 3101003, 3101001);

    REQUIRE(lc[0] == DMR_FLCO_PRIVATE);
}

TEST_CASE("Voice link control round trip", "[dmr][linkcontrol]")
{
    uint8_t lc[DMR_LC_LENGTH];
    dmr_buildVoiceLc(lc, true, 91, 3101001);

    struct dmrLinkControl parsed;
    dmr_parseLc(&parsed, lc);

    REQUIRE(parsed.flco == DMR_FLCO_GROUP);
    REQUIRE(parsed.featureSet == 0x00);
    REQUIRE(parsed.serviceOptions == 0x00);
    REQUIRE(parsed.dstId == 91);
    REQUIRE(parsed.srcId == 3101001);
}

TEST_CASE("Unknown opcodes are preserved, not dropped", "[dmr][linkcontrol]")
{
    // A talker alias header: this layer has no case for it yet, so the parse
    // must hand its octets through unharmed for a later assembler.
    const uint8_t lc[DMR_LC_LENGTH] = {
        DMR_FLCO_TA_HEADER, 0x00, 0x04, 'C', 'A', 'L', 'L', '0', '1'
    };

    struct dmrLinkControl parsed;
    dmr_parseLc(&parsed, lc);

    REQUIRE(parsed.flco == DMR_FLCO_TA_HEADER);
    REQUIRE(memcmp(parsed.payload, &lc[2], DMR_LC_LENGTH - 2) == 0);
}

TEST_CASE("Reserved opcodes parse too", "[dmr][linkcontrol]")
{
    // An opcode outside every enum value: still not dropped.
    const uint8_t lc[DMR_LC_LENGTH] = { 0x2A, 0x10, 0x01, 0x02, 0x03,
                                        0x04, 0x05, 0x06, 0x07 };

    struct dmrLinkControl parsed;
    dmr_parseLc(&parsed, lc);

    REQUIRE(parsed.flco == 0x2A);
    REQUIRE(parsed.featureSet == 0x10);
    REQUIRE(memcmp(parsed.payload, &lc[2], DMR_LC_LENGTH - 2) == 0);
}
