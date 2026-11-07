/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

extern "C" {
#include "core/stations.h"
}

#include <string>

TEST_CASE("stations: a record per callsign, newest first", "[stations]")
{
    stations_init();
    CHECK(stations_count() == 0);
    CHECK(stations_get(0) == nullptr);
    CHECK(stations_find("W1AW") == nullptr);

    station *s = stations_update("W1AW", STATION_CARRIER_APRS, 100);
    REQUIRE(s != nullptr);
    s->latitude = 42000000;
    s->valid = STATION_HAS_POSITION;
    stations_update("N2BP-7", STATION_CARRIER_APRS, 110);

    REQUIRE(stations_count() == 2);
    CHECK(std::string(stations_get(0)->call) == "N2BP-7");
    CHECK(std::string(stations_get(1)->call) == "W1AW");
    CHECK(stations_get(1)->latitude == 42000000);

    /* Heard again: to the front, fields kept, time updated */
    s = stations_update("w1aw", STATION_CARRIER_M17, 120);
    CHECK(stations_count() == 2);
    CHECK(stations_get(0) == s);
    CHECK(s->latitude == 42000000);
    CHECK(s->valid == STATION_HAS_POSITION);
    CHECK(s->lastHeard == 120);
    CHECK(s->carrier == STATION_CARRIER_M17);
    CHECK(stations_find("W1AW") == s);

    CHECK(stations_update("", STATION_CARRIER_APRS, 1) == nullptr);
    CHECK(stations_update("TOOLONGCALL", STATION_CARRIER_APRS, 1) == nullptr);
    CHECK(stations_update(nullptr, STATION_CARRIER_APRS, 1) == nullptr);
}

TEST_CASE("stations: a full table drops the oldest", "[stations]")
{
    stations_init();

    for (int i = 0; i < 70; i++) {
        std::string call = "N" + std::to_string(i);
        stations_update(call.c_str(), STATION_CARRIER_APRS, 1000 + i);
    }

    CHECK(stations_count() == 64);
    CHECK(std::string(stations_get(0)->call) == "N69");
    CHECK(std::string(stations_get(63)->call) == "N6");
    CHECK(stations_find("N5") == nullptr);

    /* Hearing an old one again keeps it from being next to go */
    stations_update("N6", STATION_CARRIER_APRS, 2000);
    stations_update("N70", STATION_CARRIER_APRS, 2001);
    CHECK(stations_find("N6") != nullptr);
    CHECK(stations_find("N7") == nullptr);
}

TEST_CASE("stations: distance and bearing", "[stations]")
{
    uint16_t b = 999;

    /* One degree along the equator and along a meridian: 111.2 km */
    uint32_t d = stations_distance(0, 0, 0, 1000000, &b);
    CHECK(d > 111000);
    CHECK(d < 111400);
    CHECK(b == 90);
    d = stations_distance(0, 0, 1000000, 0, &b);
    CHECK(d > 111000);
    CHECK(d < 111400);
    CHECK(b == 0);
    stations_distance(1000000, 0, 0, 0, &b);
    CHECK(b == 180);
    stations_distance(0, 1000000, 0, 0, &b);
    CHECK(b == 270);

    /* New York to London: 5570 km, initial bearing 51 degrees */
    d = stations_distance(40712800, -74006000, 51507400, -127800, &b);
    CHECK(d > 5560000);
    CHECK(d < 5580000);
    CHECK(b == 51);

    CHECK(stations_distance(35149000, -90049000, 35149000, -90049000, nullptr)
          == 0);
}

TEST_CASE("stations: remove and expire", "[stations]")
{
    stations_init();
    stations_update("A", STATION_CARRIER_APRS, 100);
    stations_update("B", STATION_CARRIER_APRS, 200);
    stations_update("C", STATION_CARRIER_APRS, 300);

    stations_remove("B");
    stations_remove("nobody");
    stations_remove(nullptr);
    REQUIRE(stations_count() == 2);
    CHECK(std::string(stations_get(0)->call) == "C");
    CHECK(std::string(stations_get(1)->call) == "A");

    stations_expire(1000, 800); /* A at 100 is 900 s old */
    CHECK(stations_count() == 1);
    CHECK(std::string(stations_get(0)->call) == "C");

    stations_expire(5000, 800);
    CHECK(stations_count() == 0);
}
