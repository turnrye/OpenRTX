/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Contract tests for aprsPosFormat(), the uncompressed position-report info
 * field a beacon transmits. Each test pins one property against a hand-computed
 * expected string: the DDMM.hh coordinate conversion and its two arithmetic
 * traps (32-bit overflow near a whole degree, and rounding that carries a
 * minute into the degrees), the knots and feet unit conversions, and the order
 * in which optional fields are dropped to fit a tight buffer. A final round
 * trip proves the field the formatter builds parses back as a position report
 * once it is wrapped in a real AX.25 frame.
 */

#include <catch2/catch_test_macros.hpp>

#include "protocols/APRS/packet.h"
#include <cstring>

namespace
{

/** A report with sensible defaults; tests override just what they exercise. */
struct aprsPosReport basePos()
{
    struct aprsPosReport rpt;
    rpt.latitude = 33123456;   /* 33.123456 N */
    rpt.longitude = -96987654; /* 96.987654 W */
    rpt.symbolTable = '/';
    rpt.symbolCode = '>';      /* car */
    rpt.hasCourseSpeed = false;
    rpt.course = 0;
    rpt.speed = 0;
    rpt.hasAltitude = false;
    rpt.altitude = 0;
    rpt.comment = nullptr;
    return rpt;
}

} // namespace

TEST_CASE("APRS position: a plain coordinate formats to DDMM.hh", "[aprs][pos]")
{
    struct aprsPosReport rpt = basePos();
    char out[APRS_PACLEN];

    size_t n = aprsPosFormat(out, sizeof(out), &rpt);

    /* 0.123456 deg = 7.40736', rounds to 07.41'; 0.987654 deg = 59.25924',
     * rounds to 59.26'. */
    REQUIRE(std::string(out) == "=3307.41N/09659.26W>");
    REQUIRE(n == 20);
}

TEST_CASE("APRS position: hemispheres follow the sign", "[aprs][pos]")
{
    struct aprsPosReport rpt = basePos();
    rpt.latitude = -33123456; /* south */
    rpt.longitude = 96987654; /* east  */
    char out[APRS_PACLEN];

    REQUIRE(aprsPosFormat(out, sizeof(out), &rpt) > 0);
    REQUIRE(std::string(out) == "=3307.41S/09659.26E>");
}

TEST_CASE("APRS position: rounding carries a full minute into the degrees",
          "[aprs][pos]")
{
    struct aprsPosReport rpt = basePos();
    /* 44.999917 deg -> 59.9950' which rounds to 60.00', i.e. 45 deg 00.00'. */
    rpt.latitude = 44999917;
    rpt.longitude = 0;
    char out[APRS_PACLEN];

    REQUIRE(aprsPosFormat(out, sizeof(out), &rpt) > 0);
    REQUIRE(std::string(out) == "=4500.00N/00000.00E>");
}

TEST_CASE("APRS position: the fraction scales without overflowing int32",
          "[aprs][pos]")
{
    struct aprsPosReport rpt = basePos();
    /* frac = 999999: frac*6000 would overflow a 32-bit int, frac*6/1000 does
     * not. The value also rounds up and carries, so both traps at once. */
    rpt.latitude = 12999999;
    rpt.longitude = 179999999;
    char out[APRS_PACLEN];

    REQUIRE(aprsPosFormat(out, sizeof(out), &rpt) > 0);
    REQUIRE(std::string(out) == "=1300.00N/18000.00E>");
}

TEST_CASE("APRS position: course and speed become knots", "[aprs][pos]")
{
    struct aprsPosReport rpt = basePos();
    rpt.hasCourseSpeed = true;
    rpt.course = 90;
    rpt.speed = 100; /* km/h -> 54 kn */
    char out[APRS_PACLEN];

    REQUIRE(aprsPosFormat(out, sizeof(out), &rpt) > 0);
    REQUIRE(std::string(out) == "=3307.41N/09659.26W>090/054");
}

TEST_CASE("APRS position: a course of 360 wraps to zero", "[aprs][pos]")
{
    struct aprsPosReport rpt = basePos();
    rpt.hasCourseSpeed = true;
    rpt.course = 360;
    rpt.speed = 0;
    char out[APRS_PACLEN];

    REQUIRE(aprsPosFormat(out, sizeof(out), &rpt) > 0);
    REQUIRE(std::string(out) == "=3307.41N/09659.26W>000/000");
}

TEST_CASE("APRS position: altitude becomes whole feet", "[aprs][pos]")
{
    struct aprsPosReport rpt = basePos();
    rpt.hasAltitude = true;
    rpt.altitude = 100; /* m -> 328 ft */
    char out[APRS_PACLEN];

    REQUIRE(aprsPosFormat(out, sizeof(out), &rpt) > 0);
    REQUIRE(std::string(out) == "=3307.41N/09659.26W>/A=000328");
}

TEST_CASE("APRS position: a negative altitude is clamped to zero feet",
          "[aprs][pos]")
{
    struct aprsPosReport rpt = basePos();
    rpt.hasAltitude = true;
    rpt.altitude = -50;
    char out[APRS_PACLEN];

    REQUIRE(aprsPosFormat(out, sizeof(out), &rpt) > 0);
    REQUIRE(std::string(out) == "=3307.41N/09659.26W>/A=000000");
}

TEST_CASE("APRS position: all fields appear in order", "[aprs][pos]")
{
    struct aprsPosReport rpt = basePos();
    rpt.hasCourseSpeed = true;
    rpt.course = 90;
    rpt.speed = 100;
    rpt.hasAltitude = true;
    rpt.altitude = 100;
    rpt.comment = "hello";
    char out[APRS_PACLEN];

    REQUIRE(aprsPosFormat(out, sizeof(out), &rpt) > 0);
    REQUIRE(std::string(out) == "=3307.41N/09659.26W>090/054/A=000328hello");
}

TEST_CASE("APRS position: optional fields drop comment, then altitude, "
          "then course/speed",
          "[aprs][pos]")
{
    struct aprsPosReport rpt = basePos();
    rpt.hasCourseSpeed = true;
    rpt.course = 90;
    rpt.speed = 100;
    rpt.hasAltitude = true;
    rpt.altitude = 100;
    rpt.comment = "hello";

    /* Coordinate(20) + course/speed(7) + altitude(9) = 36 chars. A buffer with
     * room for exactly that plus a NUL drops the comment but keeps the rest. */
    SECTION("comment dropped first")
    {
        char out[37];
        REQUIRE(aprsPosFormat(out, sizeof(out), &rpt) == 36);
        REQUIRE(std::string(out) == "=3307.41N/09659.26W>090/054/A=000328");
    }

    /* Coordinate(20) + course/speed(7) = 27 chars; no room for altitude. */
    SECTION("altitude dropped next")
    {
        char out[28];
        REQUIRE(aprsPosFormat(out, sizeof(out), &rpt) == 27);
        REQUIRE(std::string(out) == "=3307.41N/09659.26W>090/054");
    }

    /* Room for the coordinate alone. */
    SECTION("course and speed dropped last")
    {
        char out[21];
        REQUIRE(aprsPosFormat(out, sizeof(out), &rpt) == 20);
        REQUIRE(std::string(out) == "=3307.41N/09659.26W>");
    }
}

TEST_CASE("APRS position: a comment is truncated rather than dropping fields",
          "[aprs][pos]")
{
    struct aprsPosReport rpt = basePos();
    rpt.hasCourseSpeed = true;
    rpt.course = 90;
    rpt.speed = 100;
    rpt.comment = "truncate me";

    /* Coordinate(20) + course/speed(7) = 27, then four comment bytes fit. */
    char out[32];
    REQUIRE(aprsPosFormat(out, sizeof(out), &rpt) == 31);
    REQUIRE(std::string(out) == "=3307.41N/09659.26W>090/054trun");
}

TEST_CASE("APRS position: a buffer too small for the coordinate is refused",
          "[aprs][pos]")
{
    struct aprsPosReport rpt = basePos();
    char out[20]; /* 20 chars need 21 bytes with the NUL */

    REQUIRE(aprsPosFormat(out, sizeof(out), &rpt) == 0);
}

TEST_CASE("APRS position: out-of-range coordinates are refused", "[aprs][pos]")
{
    char out[APRS_PACLEN];

    SECTION("latitude past the pole")
    {
        struct aprsPosReport rpt = basePos();
        rpt.latitude = 90000001;
        REQUIRE(aprsPosFormat(out, sizeof(out), &rpt) == 0);
    }

    SECTION("longitude past the antimeridian")
    {
        struct aprsPosReport rpt = basePos();
        rpt.longitude = -180000001;
        REQUIRE(aprsPosFormat(out, sizeof(out), &rpt) == 0);
    }
}

TEST_CASE("APRS position: null arguments are refused", "[aprs][pos]")
{
    struct aprsPosReport rpt = basePos();
    char out[APRS_PACLEN];

    REQUIRE(aprsPosFormat(nullptr, sizeof(out), &rpt) == 0);
    REQUIRE(aprsPosFormat(out, sizeof(out), nullptr) == 0);
}

TEST_CASE("APRS position: the field wraps into a frame that parses back",
          "[aprs][pos]")
{
    struct aprsPosReport rpt = basePos();
    rpt.hasCourseSpeed = true;
    rpt.course = 90;
    rpt.speed = 100;
    rpt.comment = "OpenRTX";

    char info[APRS_PACLEN];
    size_t infoLen = aprsPosFormat(info, sizeof(info), &rpt);
    REQUIRE(infoLen > 0);

    uint8_t frame[APRS_PACLEN];
    ssize_t frameLen = aprsFrame_build(frame, sizeof(frame), APRS_TOCALL,
                                       "N0CALL-7", APRS_DEFAULT_PATH, info,
                                       infoLen);
    REQUIRE(frameLen > 0);

    struct aprsPacket pkt;
    REQUIRE(aprsPkt_fromFrame(frame, frameLen, &pkt) == 0);
    REQUIRE(pkt.type == APRS_TYPE_POSITION);
    REQUIRE(std::string(pkt.info, pkt.infoLen) == std::string(info));
}
