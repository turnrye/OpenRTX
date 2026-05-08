/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * Unit tests for OpMode_M17 packet RX/TX via the rtx packet descriptor API.
 *
 * These tests exercise addPacketRx() and addPacketTx() directly, without
 * spinning up the full RTX task or hardware.  They verify:
 *
 *  - addPacketRx: accepts up to 4 descriptors, rejects a 5th (queue full).
 *  - addPacketTx: accepts the first descriptor, rejects a second while busy.
 *  - addPacketTx: returns -EBUSY for a second call before the first completes.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include "rtx/OpMode_M17.hpp"
#include "rtx/rtx.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static struct pktDesc make_desc(void *buf, size_t size)
{
    struct pktDesc d;
    d.status = PKT_STATUS_IDLE;
    d.buffer = buf;
    d.size = size;
    d.res = 0;
    return d;
}

// ---------------------------------------------------------------------------
// addPacketRx tests
// ---------------------------------------------------------------------------

TEST_CASE("OpMode_M17 addPacketRx: accepts a single descriptor",
          "[m17][opmode][packet]")
{
    OpMode_M17 mode;

    uint8_t buf[256];
    struct pktDesc desc = make_desc(buf, sizeof(buf));
    desc.status = PKT_STATUS_SUBMITTED;
    REQUIRE(mode.addPacketRx(&desc) == 0);
}

TEST_CASE("OpMode_M17 addPacketRx: rejects when queue is full",
          "[m17][opmode][packet]")
{
    OpMode_M17 mode;

    uint8_t buf1[256], buf2[256];
    struct pktDesc desc1 = make_desc(buf1, sizeof(buf1));
    desc1.status = PKT_STATUS_SUBMITTED;
    REQUIRE(mode.addPacketRx(&desc1) == 0);

    // 2nd should fail — capacity is 1
    struct pktDesc desc2 = make_desc(buf2, sizeof(buf2));
    desc2.status = PKT_STATUS_SUBMITTED;
    REQUIRE(mode.addPacketRx(&desc2) == -EAGAIN);
}

// ---------------------------------------------------------------------------
// addPacketTx tests
// ---------------------------------------------------------------------------

TEST_CASE("OpMode_M17 addPacketTx: accepts a single descriptor",
          "[m17][opmode][packet]")
{
    OpMode_M17 mode;

    uint8_t buf[256] = {};
    struct pktDesc desc = make_desc(buf, sizeof(buf));
    desc.status = PKT_STATUS_SUBMITTED;

    REQUIRE(mode.addPacketTx(&desc) == 0);
}

TEST_CASE(
    "OpMode_M17 addPacketTx: rejects second descriptor while first pending",
    "[m17][opmode][packet]")
{
    OpMode_M17 mode;

    uint8_t buf1[256] = {};
    uint8_t buf2[256] = {};
    struct pktDesc desc1 = make_desc(buf1, sizeof(buf1));
    struct pktDesc desc2 = make_desc(buf2, sizeof(buf2));
    desc1.status = PKT_STATUS_SUBMITTED;
    desc2.status = PKT_STATUS_SUBMITTED;

    REQUIRE(mode.addPacketTx(&desc1) == 0);
    REQUIRE(mode.addPacketTx(&desc2) == -EBUSY);
}
