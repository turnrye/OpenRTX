/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include "protocols/APRS/packet.h"
#include <cstring>
#include <string>

namespace
{

aprsPacket packetWith(const char *info, const char *dest = "APRS")
{
    // The parsed packet points into the frame: a few are kept, so that
    // packets parsed one after another stay valid together
    static uint8_t frames[4][APRS_PACLEN];
    static size_t nextFrame;
    uint8_t *frame = frames[nextFrame++ % 4];
    ssize_t n = aprsFrame_build(frame, APRS_PACLEN, dest, "N2BP-7", nullptr,
                                info, strlen(info));
    REQUIRE(n > 0);
    aprsPacket pkt;
    REQUIRE(aprsPkt_fromFrame(frame, n, &pkt) == 0);
    return pkt;
}

/* The packet outlives the call: the comment points into it. */
aprsPosition parse(const char *info, const char *dest = "APRS")
{
    static aprsPacket pkt;
    pkt = packetWith(info, dest);
    aprsPosition pos;
    INFO("info: " << info);
    REQUIRE(aprsPosParse(&pkt, &pos) == true);
    return pos;
}

} // namespace

TEST_CASE("APRS position: uncompressed reports", "[aprs][pos]")
{
    /* Examples from APRS 1.2 chapter 8. */
    aprsPosition p = parse("!4903.50N/07201.75W-Test 001234");
    CHECK(p.latitude == 49058333);
    CHECK(p.longitude == -72029167);
    CHECK(p.symbolTable == '/');
    CHECK(p.symbolCode == '-');
    CHECK(p.hasCourseSpeed == false);
    CHECK(p.hasAltitude == false);
    CHECK(p.ambiguous == false);
    CHECK(p.name[0] == '\0');
    CHECK(std::string(p.comment, p.commentLen) == "Test 001234");

    p = parse("=4903.50N/07201.75W-");
    CHECK(p.latitude == 49058333);
    CHECK(std::string(p.comment, p.commentLen) == "");

    p = parse("/092345z4903.50N/07201.75W>Test1234");
    CHECK(p.symbolCode == '>');
    CHECK(std::string(p.comment, p.commentLen) == "Test1234");

    p = parse("@092345/4903.50S\\07201.75E>088/036");
    CHECK(p.latitude == -49058333);
    CHECK(p.longitude == 72029167);
    CHECK(p.symbolTable == '\\');
    CHECK(p.hasCourseSpeed == true);
    CHECK(p.course == 88);
    CHECK(p.speed == 67); /* 36 knots */
    CHECK(std::string(p.comment, p.commentLen) == "");
}

TEST_CASE("APRS position: ambiguity, altitude and unknown course",
          "[aprs][pos]")
{
    aprsPosition p = parse("!4903.5 N/07201.  W-");
    CHECK(p.ambiguous == true);
    CHECK(p.latitude == 49058333);
    CHECK(p.longitude == -72016667);

    p = parse("!4903.50N/07201.75W-/A=001234 up high");
    CHECK(p.hasAltitude == true);
    CHECK(p.altitude == 376);
    CHECK(std::string(p.comment, p.commentLen) == "/A=001234 up high");

    p = parse("!4903.50N/07201.75W>000/000/A=-00100");
    CHECK(p.hasCourseSpeed == false);
    CHECK(p.hasAltitude == true);
    CHECK(p.altitude == -30);

    p = parse("!4903.50N/07201.75W>.../...");
    CHECK(p.hasCourseSpeed == false);
    CHECK(std::string(p.comment, p.commentLen) == "");

    /* A PHG extension in the course/speed slot is not a course */
    p = parse("!4903.50N/07201.75W>PHG5132");
    CHECK(p.hasCourseSpeed == false);
    CHECK(p.hasPhg == true);
    CHECK(std::string(p.comment, p.commentLen) == "");
}

TEST_CASE("APRS position: compressed reports", "[aprs][pos]")
{
    /* The spec's worked example: 49.5N 72.75W, course 88, 36.2 knots. */
    aprsPosition p = parse("=/5L!!<*e7>7P[");
    CHECK(p.latitude == 49500000);
    CHECK(p.longitude / 100 == -727500); /* -72.75004: base-91 steps */
    CHECK(p.symbolTable == '/');
    CHECK(p.symbolCode == '>');
    CHECK(p.hasCourseSpeed == true);
    CHECK(p.course == 88);
    CHECK(p.speed == 67);

    /* No course or speed, altitude 10004 ft */
    p = parse("=/5L!!<*e7>S]  ");
    CHECK(p.hasCourseSpeed == false);
    CHECK(p.hasAltitude == false);
    p = parse("=/5L!!<*e7>S]S!");
    CHECK(p.hasAltitude == true);
    CHECK(p.altitude == 3049);
}

TEST_CASE("APRS position: Mic-E", "[aprs][pos]")
{
    /* APRS 1.2 chapter 10: S32U6T is 33 25.64N, west, offset +0, bits
     * 1/0/0; the info example decodes to 7.74 minutes, 20 knots, 251 deg,
     * a jeep. */
    aprsPosition p = parse("`(_fn\"Oj/", "S32U6T");
    CHECK(p.latitude == 33427333);
    CHECK(p.longitude == -12129000);
    CHECK(p.symbolTable == '/');
    CHECK(p.symbolCode == 'j');
    CHECK(p.hasCourseSpeed == true);
    CHECK(p.course == 251);
    CHECK(p.speed == 37);
    CHECK(p.emergency == false);
    CHECK(p.ambiguous == false);
    CHECK(p.hasAltitude == false);
    CHECK(std::string(p.comment, p.commentLen) == "");

    /* The spec decodes the same info with a +100 offset: 112 degrees */
    p = parse("`(_fn\"Oj/", "S32UVT");
    CHECK(p.longitude == -112129000);

    /* Southern and eastern hemisphere, old data, altitude, status */
    p = parse("'(_fn\"Oj/\"4T}Hello", "332564");
    CHECK(p.latitude == -33427333);
    CHECK(p.longitude == 12129000);
    CHECK(p.hasAltitude == true);
    CHECK(p.altitude == 61);
    CHECK(std::string(p.comment, p.commentLen) == "Hello");

    /* Ambiguity: T4SQZZ is 44 31.__ with the longitude hundredths dropped */
    p = parse("`(_fn\"Oj/", "T4SQZZ");
    CHECK(p.ambiguous == true);
    CHECK(p.latitude == 44516667);
    CHECK(p.longitude == -112116667);

    /* All comment bits clear is the Emergency comment */
    p = parse("`(_fn\"Oj/", "234U6T");
    CHECK(p.emergency == true);

    /* No motion */
    p = parse("`(_f\x1c\x1c\x1cj/", "S32U6T");
    CHECK(p.hasCourseSpeed == false);

    aprsPacket bad = packetWith("`(_fn\"Oj/", "APRS");
    aprsPosition pos;
    CHECK(aprsPosParse(&bad, &pos) == false);
    bad = packetWith("`(_f", "S32U6T");
    CHECK(aprsPosParse(&bad, &pos) == false);
}

TEST_CASE("APRS position: objects and items", "[aprs][pos]")
{
    aprsPosition p = parse(";LEADER   *092345z4903.50N/07201.75W>088/036");
    CHECK(std::string(p.name) == "LEADER");
    CHECK(p.killed == false);
    CHECK(p.latitude == 49058333);
    CHECK(p.course == 88);

    p = parse(";LEADER   _092345z4903.50N/07201.75W>");
    CHECK(p.killed == true);

    p = parse(")AID #2!4903.50N/07201.75W!");
    CHECK(std::string(p.name) == "AID #2");
    CHECK(p.symbolCode == '!');
    CHECK(p.killed == false);

    p = parse(")G/WB4APR_4903.50N/07201.75W-");
    CHECK(std::string(p.name) == "G/WB4APR");
    CHECK(p.killed == true);
}

TEST_CASE("APRS position: what is not a position", "[aprs][pos]")
{
    const char *none[] = { ">status",
                           ":N0CALL   :msg",
                           "!4903.50X/07201.75W-",
                           "!4963.50N/07201.75W-",
                           "!4903.50N/07201.75",
                           "=",
                           ";LEADER   *092345z",
                           ")AB!4903.50N/07201.75W-",
                           "T#005,199,000,255,073,123,01101001" };
    for (const char *info : none) {
        aprsPacket pkt = packetWith(info);
        aprsPosition pos;
        INFO("info: " << info);
        CHECK(aprsPosParse(&pkt, &pos) == false);
    }
}

TEST_CASE("APRS position: what the formatter writes parses back", "[aprs][pos]")
{
    aprsPosReport rpt = {};
    rpt.latitude = 35149000;
    rpt.longitude = -90049000;
    rpt.symbolTable = '/';
    rpt.symbolCode = '[';
    rpt.hasCourseSpeed = true;
    rpt.course = 270;
    rpt.speed = 50;
    rpt.hasAltitude = true;
    rpt.altitude = 100;
    rpt.comment = "OpenRTX";

    char info[APRS_PACLEN];
    REQUIRE(aprsPosFormat(info, sizeof(info), &rpt) > 0);

    aprsPosition p = parse(info);
    CHECK(p.latitude / 1000 == 35149); /* hundredths of a minute */
    CHECK(p.longitude / 1000 == -90049);
    CHECK(p.symbolCode == '[');
    CHECK(p.course == 270);
    CHECK(p.speed == 50);
    CHECK(p.altitude == 100);
    CHECK(std::string(p.comment, p.commentLen) == "/A=000328OpenRTX");
}

TEST_CASE("APRS position: PHG and RNG data extensions", "[aprs][pos]")
{
    /* APRS 1.0 chapter 7: PHG5132 is 25 W, 20 ft, 3 dB, favouring east */
    aprsPosition p = parse("!4903.50N/07201.75W#PHG5132 hilltop");
    CHECK(p.hasPhg == true);
    CHECK(p.phgPower == 25);
    CHECK(p.phgHeight == 20);
    CHECK(p.phgGain == 3);
    CHECK(p.phgDir == 90);
    CHECK(p.hasCourseSpeed == false);
    CHECK(std::string(p.comment, p.commentLen) == " hilltop");

    p = parse("!4903.50N/07201.75W#RNG0050");
    CHECK(p.range == 50);
    CHECK(p.hasPhg == false);
    CHECK(std::string(p.comment, p.commentLen) == "");

    /* Not an extension: the text stays in the comment */
    p = parse("!4903.50N/07201.75W#PHGabcd");
    CHECK(p.hasPhg == false);
    CHECK(std::string(p.comment, p.commentLen) == "PHGabcd");
}

TEST_CASE("APRS position: !DAO! adds a digit of precision", "[aprs][pos]")
{
    /* Human readable: thousandths of a minute */
    aprsPosition p = parse("!4903.50N/07201.75W-comment !W12!");
    CHECK(p.hasDao == true);
    CHECK(p.latitude == 49058333 + 17);
    CHECK(p.longitude == -72029167 - 33);

    /* Base 91: (c - 33) / 91 hundredths of a minute */
    p = parse("!4903.50N/07201.75W-!wJ5!");
    CHECK(p.hasDao == true);
    CHECK(p.latitude == 49058333 + 75);
    CHECK(p.longitude == -72029167 - 37);

    p = parse("!4903.50N/07201.75W-wow! not one!");
    CHECK(p.hasDao == false);
    CHECK(p.latitude == 49058333);
}
