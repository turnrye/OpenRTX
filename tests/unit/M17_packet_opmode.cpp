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

TEST_CASE("OpMode_M17 addPacketRx: accepts up to 4 descriptors",
          "[m17][opmode][packet]")
{
    OpMode_M17 mode;

    uint8_t bufs[4][256];
    struct pktDesc descs[4];

    for (int i = 0; i < 4; i++) {
        descs[i] = make_desc(bufs[i], sizeof(bufs[i]));
        descs[i].status = PKT_STATUS_SUBMITTED;
        REQUIRE(mode.addPacketRx(&descs[i]) == 0);
    }
}

TEST_CASE("OpMode_M17 addPacketRx: rejects when queue is full",
          "[m17][opmode][packet]")
{
    OpMode_M17 mode;

    uint8_t bufs[5][256];
    struct pktDesc descs[5];

    for (int i = 0; i < 4; i++) {
        descs[i] = make_desc(bufs[i], sizeof(bufs[i]));
        descs[i].status = PKT_STATUS_SUBMITTED;
        REQUIRE(mode.addPacketRx(&descs[i]) == 0);
    }

    // 5th should fail
    descs[4] = make_desc(bufs[4], sizeof(bufs[4]));
    descs[4].status = PKT_STATUS_SUBMITTED;
    REQUIRE(mode.addPacketRx(&descs[4]) == -EAGAIN);
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
