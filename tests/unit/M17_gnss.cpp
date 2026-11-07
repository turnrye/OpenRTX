/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include "protocols/M17/LinkSetupFrame.hpp"
#include <cstdlib>
#include <cstring>

using namespace M17;

namespace
{

gps_t fix(int32_t lat, int32_t lon, int16_t alt, uint16_t speed, int16_t course)
{
    gps_t g;
    memset(&g, 0, sizeof(g));
    g.fix_type = FIX_TYPE_3D;
    g.latitude = lat;
    g.longitude = lon;
    g.altitude = alt;
    g.speed = speed;
    g.tmg_true = course;
    return g;
}

} // namespace

TEST_CASE("M17 GNSS metadata reads back what was set", "[m17][gnss]")
{
    LinkSetupFrame lsf;
    lsf.clear();

    gps_t g = fix(35149000, -90049000, 100, 50, 270);
    lsf.setGnssData(&g, GNSS_STATION_HANDHELD);

    gnssPosition_t p;
    REQUIRE(lsf.getGnssData(&p) == true);
    CHECK(p.coordsValid == true);
    CHECK(p.altValid == true);
    CHECK(p.velocityValid == true);
    /* Q1.23 keeps about 1e-5 of a degree */
    CHECK(std::abs(p.latitude - 35149000) < 20);
    CHECK(std::abs(p.longitude - (-90049000)) < 40);
    CHECK(p.altitude == 100);
    CHECK(p.speed == 50);
    CHECK(p.bearing == 270);

    /* Southern and eastern hemispheres, below sea level, standing still */
    g = fix(-33867000, 151207000, -10, 0, 0);
    lsf.setGnssData(&g, GNSS_STATION_HANDHELD);
    REQUIRE(lsf.getGnssData(&p) == true);
    CHECK(std::abs(p.latitude - (-33867000)) < 20);
    CHECK(std::abs(p.longitude - 151207000) < 40);
    CHECK(p.altitude == -10);
    CHECK(p.speed == 0);
    CHECK(p.bearing == 0);
}

TEST_CASE("M17 GNSS metadata is only read from a GNSS meta field",
          "[m17][gnss]")
{
    LinkSetupFrame lsf;
    lsf.clear();

    gnssPosition_t p;
    CHECK(lsf.getGnssData(&p) == false);

    /* No fix: nothing is set, the frame stays as it was */
    gps_t g = fix(35149000, -90049000, 100, 50, 270);
    g.fix_type = FIX_TYPE_NOT_AVAIL;
    lsf.setGnssData(&g, GNSS_STATION_HANDHELD);
    CHECK(lsf.getGnssData(&p) == false);
}
