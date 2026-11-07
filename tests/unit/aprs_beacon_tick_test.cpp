/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Contract tests for aprsBeacon_tick(), the orchestration that ties the
 * scheduler, the position formatter and the transmit producer into one call.
 * The transmit path is the shared rtx packet stub, so each test can check that
 * a due beacon is rendered from the live fix and transmitted, that a beacon
 * that goes out advances the reference point while one that is refused does
 * not, and that nothing is due — or recorded — when it should not be.
 */

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <string>

extern "C" {
#include "core/aprs_beacon.h"
}
#include "rtx_packet_stub.h"

namespace
{

void tickFreshStart(int txResult)
{
    struct pktDesc *last = rtxStub_txDesc();
    if (last != nullptr)
        last->status = PKT_STATUS_DONE;

    rtxStub_reset();
    rtxStub_setTxResult(txResult);
}

struct aprsBeaconParams makeParams(enum beaconMethod method)
{
    struct aprsBeaconParams p;
    p.sched.method = method;
    p.sched.minInterval = 60;
    p.sched.maxInterval = 1800;
    p.sched.errThreshold = 600;
    p.callsign = "W1AW-9";
    p.path = "WIDE1-1,WIDE2-1";
    p.symbolTable = '/';
    p.symbolCode = '>';
    p.comment = nullptr;
    p.sendCourseSpeed = false;
    p.sendAltitude = false;
    return p;
}

struct beaconFix makeFix()
{
    struct beaconFix f;
    f.valid = true;
    f.latitude = 33123456;   /* 33.123456 N -> 3307.41N */
    f.longitude = -96987654; /* 96.987654 W -> 09659.26W */
    f.course = 90;
    f.speed = 100;
    f.altitude = 100;
    return f;
}

struct beaconState freshState()
{
    struct beaconState s;
    beacon_reset(&s);
    return s;
}

} // namespace

TEST_CASE("APRS beacon tick: nothing due does not transmit or record",
          "[aprs][beacon][tick]")
{
    tickFreshStart(0);
    struct aprsBeaconParams p = makeParams(BEACON_OFF);
    struct beaconFix fix = makeFix();
    struct beaconState st = freshState();

    REQUIRE(aprsBeacon_tick(&p, &fix, &st, 1000) == -EAGAIN);
    REQUIRE(rtxStub_txSubmissions() == 0);
    REQUIRE(st.everSent == false);
}

TEST_CASE("APRS beacon tick: no fix is never due", "[aprs][beacon][tick]")
{
    tickFreshStart(0);
    struct aprsBeaconParams p = makeParams(BEACON_INTERVAL);
    struct beaconFix fix = makeFix();
    fix.valid = false;
    struct beaconState st = freshState();

    REQUIRE(aprsBeacon_tick(&p, &fix, &st, 1000) == -EAGAIN);
    REQUIRE(rtxStub_txSubmissions() == 0);
    REQUIRE(st.everSent == false);
}

TEST_CASE("APRS beacon tick: a due beacon is rendered, sent and recorded",
          "[aprs][beacon][tick]")
{
    tickFreshStart(0);
    struct aprsBeaconParams p = makeParams(BEACON_INTERVAL);
    p.sendCourseSpeed = true;
    p.sendAltitude = true;
    p.comment = "hi";
    struct beaconFix fix = makeFix();
    struct beaconState st = freshState(); /* first beacon fires immediately */

    REQUIRE(aprsBeacon_tick(&p, &fix, &st, 4242) == 0);
    REQUIRE(rtxStub_txSubmissions() == 1);

    /* The reference point advanced to this fix and time. */
    REQUIRE(st.everSent == true);
    REQUIRE(st.lastTxTime == 4242);
    REQUIRE(st.lastTxLat == fix.latitude);
    REQUIRE(st.lastTxLon == fix.longitude);

    /* The transmitted frame carries the fix as a position report. */
    struct pktDesc *d = rtxStub_txDesc();
    REQUIRE(d != nullptr);
    struct aprsPacket pkt;
    REQUIRE(aprsPkt_fromFrame((const uint8_t *)d->buffer, d->size, &pkt) == 0);
    REQUIRE(pkt.type == APRS_TYPE_POSITION);

    char addr[APRS_ADDR_STR_LEN];
    aprsAddr_toStr(&pkt.addresses[1], addr, sizeof(addr));
    REQUIRE(std::string(addr) == "W1AW-9");
    REQUIRE(std::string(pkt.info, pkt.infoLen)
            == "=3307.41N/09659.26W>090/054/A=000328hi");
}

TEST_CASE("APRS beacon tick: the factory callsign transmits nothing",
          "[aprs][beacon][tick]")
{
    tickFreshStart(0);
    struct aprsBeaconParams p = makeParams(BEACON_INTERVAL);
    p.callsign = "N0CALL";
    struct beaconFix fix = makeFix();
    struct beaconState st = freshState();

    REQUIRE(aprsBeacon_tick(&p, &fix, &st, 1000) == -EINVAL);
    REQUIRE(rtxStub_txSubmissions() == 0);
    /* A beacon that never went out must not advance the reference point. */
    REQUIRE(st.everSent == false);
}

TEST_CASE("APRS beacon tick: a busy slot is not recorded, so it retries",
          "[aprs][beacon][tick]")
{
    tickFreshStart(-EBUSY);
    struct aprsBeaconParams p = makeParams(BEACON_INTERVAL);
    struct beaconFix fix = makeFix();
    struct beaconState st = freshState();

    REQUIRE(aprsBeacon_tick(&p, &fix, &st, 1000) == -EBUSY);
    REQUIRE(st.everSent == false);

    /* Next tick, slot free: the still-due first beacon goes out. */
    tickFreshStart(0);
    REQUIRE(aprsBeacon_tick(&p, &fix, &st, 1001) == 0);
    REQUIRE(st.everSent == true);
    REQUIRE(st.lastTxTime == 1001);
}

TEST_CASE("APRS beacon tick: an explicit request beacons even when off",
          "[aprs][beacon][tick]")
{
    tickFreshStart(0);
    struct aprsBeaconParams p = makeParams(BEACON_OFF);
    struct beaconFix fix = makeFix();
    struct beaconState st = freshState();

    /* Off: nothing is due. */
    REQUIRE(aprsBeacon_tick(&p, &fix, &st, 1000) == -EAGAIN);
    REQUIRE(rtxStub_txSubmissions() == 0);

    /* Request a beacon now: the next tick sends despite the off method. */
    aprsBeacon_requestNow();
    REQUIRE(aprsBeacon_tick(&p, &fix, &st, 1001) == 0);
    REQUIRE(rtxStub_txSubmissions() == 1);
    REQUIRE(st.everSent == true);

    /* One-shot: it does not keep beaconing after the request is served. */
    REQUIRE(aprsBeacon_tick(&p, &fix, &st, 1002) == -EAGAIN);
    REQUIRE(rtxStub_txSubmissions() == 1);
}

TEST_CASE("APRS beacon tick: last-sent age tracks the send",
          "[aprs][beacon][tick]")
{
    tickFreshStart(0);
    struct aprsBeaconParams p = makeParams(BEACON_INTERVAL);
    struct beaconFix fix = makeFix();
    struct beaconState st = freshState();

    REQUIRE(aprsBeacon_tick(&p, &fix, &st, 5000) == 0);

    uint32_t age = 999;
    REQUIRE(aprsBeacon_lastSent(&age, 5000) == true);
    REQUIRE(age == 0);
    REQUIRE(aprsBeacon_lastSent(&age, 5042) == true);
    REQUIRE(age == 42);
}

TEST_CASE("APRS beacon tick: null arguments are refused",
          "[aprs][beacon][tick]")
{
    struct aprsBeaconParams p = makeParams(BEACON_INTERVAL);
    struct beaconFix fix = makeFix();
    struct beaconState st = freshState();

    REQUIRE(aprsBeacon_tick(nullptr, &fix, &st, 0) == -EINVAL);
    REQUIRE(aprsBeacon_tick(&p, nullptr, &st, 0) == -EINVAL);
    REQUIRE(aprsBeacon_tick(&p, &fix, nullptr, 0) == -EINVAL);
}
