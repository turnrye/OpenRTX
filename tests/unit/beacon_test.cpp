/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Contract tests for the beacon scheduler. The decision function takes only
 * plain values — config, last-transmitted state, current fix, and the time —
 * so every rule can be pinned exactly with no radio, GPS, or clock. The
 * adaptive cases are placed at the equator so a degree of longitude and a
 * degree of latitude cover the same ground, which keeps the expected
 * dead-reckoning distances easy to check by hand: one metre is 1e6/111320 =
 * about 8.983 microdegrees.
 */

#include <catch2/catch_test_macros.hpp>

#include "core/beacon.h"

namespace
{

/* 600 metres north of the equatorial prime meridian, in microdegrees. */
constexpr int32_t METRES_600 = 5390; /* 600 * 1e6 / 111320, rounded */

struct beaconConfig makeConfig(enum beaconMethod method)
{
    struct beaconConfig cfg;
    cfg.method = method;
    cfg.minInterval = 60;
    cfg.maxInterval = 1800;
    cfg.errThreshold = 600;
    return cfg;
}

/* A station that last beaconed at t=0 from (0,0) reporting due north at
 * 36 km/h, i.e. 10 m/s. */
struct beaconState makeState()
{
    struct beaconState st;
    st.everSent = true;
    st.forceNext = false;
    st.lastTxTime = 0;
    st.lastTxLat = 0;
    st.lastTxLon = 0;
    st.lastTxCourse = 0; /* north */
    st.lastTxSpeed = 36; /* km/h  */
    return st;
}

struct beaconFix makeFix(int32_t lat, int32_t lon)
{
    struct beaconFix fix;
    fix.valid = true;
    fix.latitude = lat;
    fix.longitude = lon;
    fix.course = 0;
    fix.speed = 36;
    fix.altitude = 0;
    return fix;
}

} // namespace

TEST_CASE("beacon: an explicit request beacons even when off", "[beacon]")
{
    struct beaconConfig off = makeConfig(BEACON_OFF);
    struct beaconState st = makeState();
    struct beaconFix fix = makeFix(0, 0);

    /* Off means silent... */
    REQUIRE(beacon_due(&off, &st, &fix, 100000) == false);

    /* ...until an explicit request overrides the method and timing. */
    st.forceNext = true;
    REQUIRE(beacon_due(&off, &st, &fix, 100000) == true);
}

TEST_CASE("beacon: a forced beacon still needs a position", "[beacon]")
{
    struct beaconConfig cfg = makeConfig(BEACON_INTERVAL);
    struct beaconState st = makeState();
    st.forceNext = true;
    struct beaconFix fix = makeFix(0, 0);
    fix.valid = false;

    REQUIRE(beacon_due(&cfg, &st, &fix, 0) == false);
}

TEST_CASE("beacon: recording a beacon clears the force", "[beacon]")
{
    struct beaconState st = makeState();
    st.forceNext = true;
    struct beaconFix fix = makeFix(0, 0);

    beacon_sent(&st, &fix, 1000);
    REQUIRE(st.forceNext == false);
}

TEST_CASE("beacon: off and manual never fire on the timer", "[beacon]")
{
    struct beaconState st = makeState();
    struct beaconFix fix = makeFix(0, 0);

    struct beaconConfig off = makeConfig(BEACON_OFF);
    struct beaconConfig man = makeConfig(BEACON_MANUAL);

    /* Long after the heartbeat would have fired for an automatic method. */
    REQUIRE(beacon_due(&off, &st, &fix, 100000) == false);
    REQUIRE(beacon_due(&man, &st, &fix, 100000) == false);
}

TEST_CASE("beacon: nothing is due without a fix", "[beacon]")
{
    struct beaconConfig cfg = makeConfig(BEACON_INTERVAL);
    struct beaconState st = makeState();
    struct beaconFix fix = makeFix(0, 0);
    fix.valid = false;

    REQUIRE(beacon_due(&cfg, &st, &fix, 100000) == false);

    /* Not even a first beacon with no position to report. */
    st.everSent = false;
    REQUIRE(beacon_due(&cfg, &st, &fix, 0) == false);
}

TEST_CASE("beacon: the first beacon fires as soon as there is a fix",
          "[beacon]")
{
    struct beaconState st = makeState();
    st.everSent = false;
    struct beaconFix fix = makeFix(0, 0);

    struct beaconConfig interval = makeConfig(BEACON_INTERVAL);
    struct beaconConfig adaptive = makeConfig(BEACON_ADAPTIVE);

    REQUIRE(beacon_due(&interval, &st, &fix, 0) == true);
    REQUIRE(beacon_due(&adaptive, &st, &fix, 0) == true);
}

TEST_CASE("beacon: interval fires once the minimum interval elapses",
          "[beacon]")
{
    struct beaconConfig cfg = makeConfig(BEACON_INTERVAL);
    struct beaconState st = makeState();
    struct beaconFix fix = makeFix(0, 0);

    REQUIRE(beacon_due(&cfg, &st, &fix, 59) == false);
    REQUIRE(beacon_due(&cfg, &st, &fix, 60) == true);
}

TEST_CASE("beacon: the heartbeat fires even when nothing has moved", "[beacon]")
{
    struct beaconConfig cfg = makeConfig(BEACON_ADAPTIVE);
    struct beaconState st = makeState();
    st.lastTxSpeed = 0; /* stationary */
    struct beaconFix fix = makeFix(0, 0);
    fix.speed = 0;

    /* Well past the rate floor but short of the heartbeat, and not moving. */
    REQUIRE(beacon_due(&cfg, &st, &fix, 300) == false);
    /* At the heartbeat it fires regardless. */
    REQUIRE(beacon_due(&cfg, &st, &fix, 1800) == true);
}

TEST_CASE("beacon: adaptive stays quiet on a predictable straight line",
          "[beacon]")
{
    struct beaconConfig cfg = makeConfig(BEACON_ADAPTIVE);
    struct beaconState st = makeState();

    /* 60 s at 10 m/s due north is 600 m; the fix is exactly there, so a
     * listener's dead reckoning already has it right. */
    struct beaconFix fix = makeFix(METRES_600, 0);

    REQUIRE(beacon_due(&cfg, &st, &fix, 60) == false);
}

TEST_CASE("beacon: adaptive fires when the station stops unexpectedly",
          "[beacon]")
{
    struct beaconConfig cfg = makeConfig(BEACON_ADAPTIVE);
    struct beaconState st = makeState();

    /* Reported moving north at 10 m/s but actually still at the origin: a
     * listener now places it 600 m away, exactly the threshold. */
    struct beaconFix fix = makeFix(0, 0);
    fix.speed = 0;

    REQUIRE(beacon_due(&cfg, &st, &fix, 60) == true);
}

TEST_CASE("beacon: adaptive fires when the station turns", "[beacon]")
{
    struct beaconConfig cfg = makeConfig(BEACON_ADAPTIVE);
    struct beaconState st = makeState();

    /* Predicted 600 m north, actually went 600 m east: the error is the
     * hypotenuse, about 848 m, well over the threshold. */
    struct beaconFix fix = makeFix(0, METRES_600);
    fix.course = 90;

    REQUIRE(beacon_due(&cfg, &st, &fix, 60) == true);
}

TEST_CASE("beacon: the rate floor blocks even a large error", "[beacon]")
{
    struct beaconConfig cfg = makeConfig(BEACON_ADAPTIVE);
    struct beaconState st = makeState();

    /* A whole degree away — a huge error — but only 30 s have passed. */
    struct beaconFix fix = makeFix(1000000, 0);

    REQUIRE(beacon_due(&cfg, &st, &fix, 30) == false);
}

TEST_CASE("beacon: recording a send moves the reference point", "[beacon]")
{
    struct beaconState st = makeState();
    st.everSent = false;

    struct beaconFix fix = makeFix(12345678, -98765432);
    fix.course = 123;
    fix.speed = 45;

    beacon_sent(&st, &fix, 4242);

    REQUIRE(st.everSent == true);
    REQUIRE(st.lastTxTime == 4242);
    REQUIRE(st.lastTxLat == 12345678);
    REQUIRE(st.lastTxLon == -98765432);
    REQUIRE(st.lastTxCourse == 123);
    REQUIRE(st.lastTxSpeed == 45);
}

TEST_CASE("beacon: reset makes the next beacon a first beacon", "[beacon]")
{
    struct beaconConfig cfg = makeConfig(BEACON_INTERVAL);
    struct beaconState st = makeState();

    beacon_reset(&st);
    REQUIRE(st.everSent == false);

    /* A first beacon fires immediately rather than waiting an interval. */
    struct beaconFix fix = makeFix(0, 0);
    REQUIRE(beacon_due(&cfg, &st, &fix, 0) == true);
}

TEST_CASE("beacon: null arguments are refused", "[beacon]")
{
    struct beaconConfig cfg = makeConfig(BEACON_INTERVAL);
    struct beaconState st = makeState();
    struct beaconFix fix = makeFix(0, 0);

    REQUIRE(beacon_due(nullptr, &st, &fix, 0) == false);
    REQUIRE(beacon_due(&cfg, nullptr, &fix, 0) == false);
    REQUIRE(beacon_due(&cfg, &st, nullptr, 0) == false);

    /* These must simply not crash. */
    beacon_sent(nullptr, &fix, 0);
    beacon_reset(nullptr);
}
