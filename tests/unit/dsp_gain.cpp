/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include "core/dsp.h"

TEST_CASE("Gain scales samples within the int16 range", "[dsp][gain]")
{
    REQUIRE(dsp_applyGain(1000, 4) == 4000);
    REQUIRE(dsp_applyGain(-1000, 4) == -4000);
    REQUIRE(dsp_applyGain(1000, 1) == 1000);
    REQUIRE(dsp_applyGain(1023, 32) == 32736);
}

TEST_CASE("Gain saturates instead of wrapping", "[dsp][gain]")
{
    REQUIRE(dsp_applyGain(10000, 4) == INT16_MAX);
    REQUIRE(dsp_applyGain(-10000, 4) == INT16_MIN);
    REQUIRE(dsp_applyGain(INT16_MAX, 32) == INT16_MAX);
    REQUIRE(dsp_applyGain(INT16_MIN, 32) == INT16_MIN);
}
