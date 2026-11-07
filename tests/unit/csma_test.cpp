/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "core/csma.h"

static constexpr uint32_t SLOT_TIME = 100;
static constexpr uint32_t DEFER_TIMEOUT = 5000;

/**
 * Set up a transmitter with the test's slot time and timeout.
 */
static void setup(struct csmaState *s, uint8_t persist, uint32_t seed)
{
    csma_init(s, persist, SLOT_TIME, DEFER_TIMEOUT, seed);
}

TEST_CASE("csma: a busy channel holds until the timeout", "[csma]")
{
    struct csmaState s;
    setup(&s, 255, 1);

    for (int64_t now = 0; now < DEFER_TIMEOUT; now += SLOT_TIME)
        REQUIRE(csma_mayTransmit(&s, true, now) == false);

    REQUIRE(csma_mayTransmit(&s, true, DEFER_TIMEOUT) == true);
}

TEST_CASE("csma: full persistence transmits on the first clear slot", "[csma]")
{
    struct csmaState s;
    setup(&s, 255, 1);

    REQUIRE(csma_mayTransmit(&s, false, 0) == true);
}

TEST_CASE("csma: partial persistence transmits within a bounded backoff",
          "[csma]")
{
    struct csmaState s;
    setup(&s, 127, 0xC0FFEE);

    // Well before the timeout, so a transmit is a draw and not the backstop
    size_t slot = 0;
    for (; slot < 40; slot++) {
        if (csma_mayTransmit(&s, false, (int64_t)(slot * SLOT_TIME)))
            break;
    }

    REQUIRE(slot < 40);
}

TEST_CASE("csma: a failed draw holds for the rest of its slot", "[csma]")
{
    // With persist 0 only a zero draw passes, and this seed's first is not
    struct csmaState s;
    setup(&s, 0, 0xABCDEF01);

    REQUIRE(csma_mayTransmit(&s, false, 0) == false);
    for (int64_t now = 1; now < SLOT_TIME; now += 10)
        REQUIRE(csma_mayTransmit(&s, false, now) == false);
}
