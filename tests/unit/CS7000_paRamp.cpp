/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>
#include "drivers/baseband/paRamp_CS7000.h"

using Table = std::vector<uint8_t>;

/*
 * Reference ramps, from the stock firmware's own ramp builder run in an
 * emulator against the calibration of a CS7000 Plus, at 435MHz: the APC value
 * of each power level, the calibrated ramp timing in units of 100us, and the
 * APC values put out on the way up and on the way down.
 */
struct Reference {
    uint8_t level;
    uint8_t start;
    uint8_t upEnd;
    uint16_t holdEnd;
    uint16_t downEnd;
    Table up;
    Table down;
    int apcOn; // Steps of 50us from the transmit interrupt
    int paOn;
    int paOff;
    int end;
};

static const Reference LOW_POWER = {
    0x67,
    11,
    18,
    306,
    315,
    { 0x00, 0x29, 0x35, 0x3A, 0x3F, 0x44, 0x49, 0x4E, 0x53, 0x58, 0x5D, 0x62,
      0x67, 0x6C },
    { 0x5F, 0x52, 0x45, 0x38, 0x31, 0x2A, 0x23, 0x1C, 0x15, 0x0E, 0x07, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00 },
    22,
    35,
    629,
    630 // 1.10ms, 1.75ms, 31.45ms, 31.50ms
};

static const Reference HIGH_POWER = {
    0xCD,
    13,
    20,
    306,
    316,
    { 0x00, 0x50, 0x69, 0x73, 0x7D, 0x87, 0x91, 0x9B, 0xA5, 0xAF, 0xB9, 0xC3,
      0xCD, 0xD7 },
    { 0xBD, 0xA3, 0x89, 0x6F, 0x62, 0x54, 0x46, 0x38, 0x2A, 0x1C, 0x0E, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    26,
    32,
    632,
    632 // 1.30ms, 1.60ms, 31.60ms, 31.60ms
};

/*
 * Steps, from the transmit interrupt, at which the ramp turns the APC switch
 * and the PA on and off, following the stock firmware: the PA comes on at its
 * step of the ramp up, or at its top if that comes first, and goes off at its
 * step of the ramp down, or at its end.
 */
static void checkRamp(const Reference &ref)
{
    struct paRampShape shape;
    REQUIRE(paRamp_build(&shape, ref.level, ref.start, ref.upEnd, ref.holdEnd,
                         ref.downEnd));

    REQUIRE(shape.upLen == ref.up.size());
    REQUIRE(Table(shape.up, shape.up + shape.upLen) == ref.up);

    // Put out from the last entry back, as the hold ends
    REQUIRE(shape.downLen == ref.down.size() + 1);
    Table down;
    for (int i = shape.downLen - 1; i > 0; i--)
        down.push_back(shape.down[i]);
    REQUIRE(down == ref.down);

    int upLast = shape.start + shape.upLen - 1;
    int paOn = std::min<int>(shape.start + shape.paOn, upLast);
    int paOff = std::min<int>(shape.holdEnd + shape.paOff,
                              shape.holdEnd + shape.downLen);

    REQUIRE(shape.start == ref.apcOn);
    REQUIRE(paOn == ref.paOn);
    REQUIRE(paOff == ref.paOff);
    REQUIRE(shape.holdEnd + shape.downLen == ref.end);
}

TEST_CASE("CS7000 Plus PA ramp matches the stock firmware at low power",
          "[cs7000][paramp]")
{
    checkRamp(LOW_POWER);
}

TEST_CASE("CS7000 Plus PA ramp matches the stock firmware at high power",
          "[cs7000][paramp]")
{
    checkRamp(HIGH_POWER);
}

TEST_CASE("CS7000 Plus PA switching steps follow the APC value",
          "[cs7000][paramp]")
{
    // APC value, then the steps turning the PA on and off: the stock tables
    // around each of their thresholds.
    const struct {
        uint8_t level, on, off;
    } points[] = { { 0x01, 36, 14 }, { 0x3B, 36, 14 }, { 0x3C, 34, 14 },
                   { 0x4A, 24, 14 }, { 0x4B, 26, 15 }, { 0x4F, 26, 15 },
                   { 0x50, 22, 15 }, { 0x5E, 17, 15 }, { 0x5F, 18, 17 },
                   { 0x64, 17, 17 }, { 0xAE, 8, 17 },  { 0xAF, 8, 18 },
                   { 0xC2, 7, 18 },  { 0xC3, 6, 20 },  { 0xF4, 4, 20 },
                   { 0xF5, 3, 20 },  { 0xFF, 3, 20 } };

    for (const auto &p : points) {
        struct paRampShape shape;
        INFO("APC " << (int)p.level);
        REQUIRE(paRamp_build(&shape, p.level, 11, 18, 306, 315));
        REQUIRE(shape.paOn == p.on);
        REQUIRE(shape.paOff == p.off);
    }
}

TEST_CASE("CS7000 Plus PA ramp stays within the APC range", "[cs7000][paramp]")
{
    for (int level = 1; level <= 0xFF; level++) {
        struct paRampShape shape;
        REQUIRE(paRamp_build(&shape, level, 11, 18, 306, 315));
        REQUIRE(shape.up[0] == 0);

        for (int i = 1; i < shape.upLen; i++) {
            REQUIRE(shape.up[i] >= shape.up[i - 1]);
            REQUIRE(shape.up[i] <= 0xFA);
        }
    }
}

TEST_CASE("CS7000 Plus PA ramp refuses unusable calibration",
          "[cs7000][paramp]")
{
    struct paRampShape shape;

    // No output power
    REQUIRE_FALSE(paRamp_build(&shape, 0x00, 11, 18, 306, 315));

    // Erased calibration
    REQUIRE_FALSE(paRamp_build(&shape, 0xFF, 0xFF, 0xFF, 0xFFFF, 0xFFFF));

    // Ramp up too short, or too long for its table
    REQUIRE_FALSE(paRamp_build(&shape, 0x67, 11, 13, 306, 315));
    REQUIRE_FALSE(paRamp_build(&shape, 0x67, 0, 33, 306, 315));

    // No ramp down
    REQUIRE_FALSE(paRamp_build(&shape, 0x67, 11, 18, 306, 306));

    // Ramp up running into the end of the hold
    REQUIRE_FALSE(paRamp_build(&shape, 0x67, 11, 18, 17, 30));

    // Ending later than a burst allows
    REQUIRE_FALSE(paRamp_build(&shape, 0x67, 11, 18, 345, 356));
}
