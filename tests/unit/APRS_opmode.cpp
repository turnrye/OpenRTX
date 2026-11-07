/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Contract tests for the APRS operating mode's packet interface, the part
 * that can be checked without a radio, an audio device or the rtx thread.
 */

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <cstring>

#include "protocols/APRS/packet.h"
#include "rtx/OpMode_APRS.hpp"
#include "rtx/rtx.h"

/**
 * A descriptor carrying a well-formed frame, ready to submit.
 */
static struct pktDesc makeDesc(uint8_t *buf, size_t cap)
{
    const char info[] = ":W1AW     :hello";
    ssize_t len = aprsFrame_build(buf, cap, APRS_TOCALL, "N0CALL-7",
                                  APRS_DEFAULT_PATH, info, strlen(info));
    REQUIRE(len > 0);

    struct pktDesc desc;
    desc.status = PKT_STATUS_SUBMITTED;
    desc.buffer = buf;
    desc.size = (size_t)len;
    desc.res = 0;
    return desc;
}

TEST_CASE("OpMode_APRS addPacketTx: a well-formed frame is accepted",
          "[aprs][opmode]")
{
    OpMode_APRS mode;
    uint8_t buf[APRS_PACLEN];
    struct pktDesc desc = makeDesc(buf, sizeof(buf));

    REQUIRE(mode.addPacketTx(&desc) == 0);
}

TEST_CASE("OpMode_APRS addPacketTx: only one transmission is in flight",
          "[aprs][opmode]")
{
    OpMode_APRS mode;
    uint8_t bufA[APRS_PACLEN];
    uint8_t bufB[APRS_PACLEN];
    struct pktDesc first = makeDesc(bufA, sizeof(bufA));
    struct pktDesc second = makeDesc(bufB, sizeof(bufB));

    REQUIRE(mode.addPacketTx(&first) == 0);

    /* Refused, not queued and not substituted: the slot is claimed with a
     * compare-and-exchange, so two threads cannot both believe they own it. */
    REQUIRE(mode.addPacketTx(&second) == -EBUSY);
}

TEST_CASE("OpMode_APRS addPacketTx: malformed requests are refused",
          "[aprs][opmode]")
{
    OpMode_APRS mode;
    uint8_t buf[APRS_PACLEN];
    struct pktDesc desc = makeDesc(buf, sizeof(buf));

    SECTION("an empty frame")
    {
        desc.size = 0;
        REQUIRE(mode.addPacketTx(&desc) == -EINVAL);
    }

    SECTION("a frame longer than AX.25 allows once the FCS is added")
    {
        desc.size = APRS_PACLEN - APRS_FCS_LEN + 1;
        REQUIRE(mode.addPacketTx(&desc) == -EINVAL);
    }

    /* A refused request must not have claimed the slot. */
    uint8_t good[APRS_PACLEN];
    struct pktDesc valid = makeDesc(good, sizeof(good));
    REQUIRE(mode.addPacketTx(&valid) == 0);
}

TEST_CASE("OpMode_APRS: the mode identifier is APRS", "[aprs][opmode]")
{
    OpMode_APRS mode;
    REQUIRE(mode.getID() == OPMODE_APRS);
}

TEST_CASE("OpMode_APRS disable: hands back queued receive descriptors",
          "[aprs][opmode]")
{
    OpMode_APRS mode;
    uint8_t bufA[APRS_PACLEN], bufB[APRS_PACLEN];
    struct pktDesc a = { PKT_STATUS_SUBMITTED, bufA, sizeof(bufA), 0 };
    struct pktDesc b = { PKT_STATUS_SUBMITTED, bufB, sizeof(bufB), 0 };

    mode.enable();
    REQUIRE(mode.addPacketRx(&a) == 0);
    REQUIRE(mode.addPacketRx(&b) == 0);

    /* Leaving the mode must not strand them: the next mode never sees them,
     * so only a handed-back descriptor can be resubmitted. */
    mode.disable();

    CHECK(a.status == PKT_STATUS_ERROR);
    CHECK(a.res == -ECANCELED);
    CHECK(b.status == PKT_STATUS_ERROR);
    CHECK(b.res == -ECANCELED);

    /* The queue is empty again */
    a.status = PKT_STATUS_SUBMITTED;
    b.status = PKT_STATUS_SUBMITTED;
    REQUIRE(mode.addPacketRx(&a) == 0);
    REQUIRE(mode.addPacketRx(&b) == 0);
}

TEST_CASE("OpMode_APRS enable: hands back descriptors queued while off",
          "[aprs][opmode]")
{
    OpMode_APRS mode;
    uint8_t buf[APRS_PACLEN];
    struct pktDesc desc = { PKT_STATUS_SUBMITTED, buf, sizeof(buf), 0 };

    REQUIRE(mode.addPacketRx(&desc) == 0);
    mode.enable();

    CHECK(desc.status == PKT_STATUS_ERROR);
    CHECK(desc.res == -ECANCELED);
    mode.disable();
}

TEST_CASE("OpMode_APRS enable: cancels a transmission queued while off",
          "[aprs][opmode]")
{
    OpMode_APRS mode;
    uint8_t buf[APRS_PACLEN];
    struct pktDesc desc = makeDesc(buf, sizeof(buf));

    REQUIRE(mode.addPacketTx(&desc) == 0);
    mode.enable();

    CHECK(desc.status == PKT_STATUS_ERROR);
    CHECK(desc.res == -ECANCELED);

    /* The slot is free again */
    struct pktDesc next = makeDesc(buf, sizeof(buf));
    REQUIRE(mode.addPacketTx(&next) == 0);
    mode.disable();
}
