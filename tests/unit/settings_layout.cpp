/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
#include "core/settings.h"
}

/*
 * The settings are stored as they sit in memory, so their layout is part of
 * what a radio reads back after an update. The DMR settings come after every
 * other field, in four bytes of ID, four of destination and one byte of bit
 * fields, so adding one in a spare bit leaves the rest where it was.
 */
static_assert(offsetof(settings_t, dmr_id)
                  == offsetof(settings_t, M17_meta_text)
                         + sizeof(settings_t::M17_meta_text),
              "the DMR settings follow the last of the other settings");
static_assert(offsetof(settings_t, dmr_talkgroup)
                  == offsetof(settings_t, dmr_id) + sizeof(uint32_t),
              "the DMR destination follows the DMR ID");
static_assert(sizeof(settings_t)
                  == offsetof(settings_t, dmr_talkgroup) + sizeof(uint32_t) + 1,
              "the DMR bit fields take a single byte at the end");

/* The byte holding the DMR bit fields, as stored. */
static uint8_t dmrFlags(const settings_t &s)
{
    uint8_t b;
    memcpy(&b, reinterpret_cast<const uint8_t *>(&s) + sizeof(settings_t) - 1,
           1);
    return b;
}

TEST_CASE("Settings keep each DMR bit field in its place", "[settings]")
{
    settings_t s;
    memset(&s, 0, sizeof(s));

    s.dmr_colorcode = 0x0F;
    REQUIRE(dmrFlags(s) == 0x0F);
    s.dmr_colorcode = 0;

    s.dmr_timeslot = 1;
    REQUIRE(dmrFlags(s) == 0x10);
    s.dmr_timeslot = 0;

    s.dmr_rx_promisc = 1;
    REQUIRE(dmrFlags(s) == 0x20);
    s.dmr_rx_promisc = 0;

    s.dmr_private_call = 1;
    REQUIRE(dmrFlags(s) == 0x40);
}

TEST_CASE("Settings default to DMR group calls", "[settings]")
{
    REQUIRE(default_settings.dmr_private_call == 0);
    REQUIRE(default_settings.dmr_talkgroup == 9);
}
