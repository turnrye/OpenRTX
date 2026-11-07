/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

extern "C" {
#include "core/aprs_traffic.h"
}

#include <cstring>
#include <string>

namespace
{

/** Parse the frame a TNC2 line describes, as the receive path would. */
aprsPacket packet(const char *src, const char *path, const char *info)
{
    // The parsed packet points into the frame: a few are kept, so that
    // packets parsed one after another stay valid together
    static uint8_t frames[4][APRS_PACLEN];
    static size_t nextFrame;
    uint8_t *frame = frames[nextFrame++ % 4];
    ssize_t n = aprsFrame_build(frame, APRS_PACLEN, "APRS", src, path, info,
                                strlen(info));
    REQUIRE(n > 0);

    aprsPacket pkt;
    REQUIRE(aprsPkt_fromFrame(frame, n, &pkt) == 0);
    return pkt;
}

} // namespace

TEST_CASE("aprs traffic: entries come back newest first as TNC2 lines",
          "[aprs][traffic]")
{
    aprsTraffic_init();
    CHECK(aprsTraffic_count() == 0);
    CHECK(aprsTraffic_get(0) == nullptr);

    aprsPacket a = packet("N2BP-7", "WIDE1-1,WIDE2-1", "!4903.50N/07201.75W-");
    aprsPacket b = packet("W1AW", nullptr, ":K1ABC    :hi{01");
    aprsTraffic_record(&a, 0, 100);
    aprsTraffic_record(&b, APRS_TRAFFIC_FOR_ME, 105);

    REQUIRE(aprsTraffic_count() == 2);
    const aprsTrafficEntry *e = aprsTraffic_get(0);
    CHECK(std::string(e->sender) == "W1AW");
    CHECK(std::string(e->line) == "W1AW>APRS::K1ABC    :hi{01");
    CHECK(e->type == APRS_TYPE_MESSAGE);
    CHECK(e->flags == APRS_TRAFFIC_FOR_ME);
    CHECK(e->heard == 105);

    e = aprsTraffic_get(1);
    CHECK(std::string(e->line)
          == "N2BP-7>APRS,WIDE1-1,WIDE2-1:!4903.50N/07201.75W-");
    CHECK(e->type == APRS_TYPE_POSITION);
    CHECK(e->flags == 0);
}

TEST_CASE("aprs traffic: a full log drops the oldest", "[aprs][traffic]")
{
    aprsTraffic_init();

    for (int i = 0; i < 40; i++) {
        std::string info = ">status " + std::to_string(i);
        aprsPacket p = packet("N2BP", nullptr, info.c_str());
        aprsTraffic_record(&p, 0, 1000 + i * 60);
    }

    CHECK(aprsTraffic_count() == 32);
    CHECK(std::string(aprsTraffic_get(0)->line) == "N2BP>APRS:>status 39");
    CHECK(std::string(aprsTraffic_get(31)->line) == "N2BP>APRS:>status 8");
    CHECK(aprsTraffic_get(32) == nullptr);
}

TEST_CASE("aprs traffic: a repeat within 30 s is marked a duplicate",
          "[aprs][traffic]")
{
    aprsTraffic_init();

    aprsPacket direct = packet("N2BP-7", "WIDE1-1", "!4903.50N/07201.75W-");
    aprsPacket digi = packet("N2BP-7", "W1AW*", "!4903.50N/07201.75W-");
    aprsPacket other = packet("N2BP-7", "WIDE1-1", "!4903.50N/07201.75W-x");

    aprsTraffic_record(&direct, 0, 100);
    aprsTraffic_record(&digi, 0, 103);
    aprsTraffic_record(&other, 0, 104);
    aprsTraffic_record(&digi, 0, 140);

    CHECK((aprsTraffic_get(3)->flags & APRS_TRAFFIC_DUP) == 0);
    CHECK((aprsTraffic_get(2)->flags & APRS_TRAFFIC_DUP) != 0);
    CHECK((aprsTraffic_get(1)->flags & APRS_TRAFFIC_DUP) == 0);
    CHECK((aprsTraffic_get(0)->flags & APRS_TRAFFIC_DUP) == 0);

    /* A message for us, heard again later, is one too. */
    aprsPacket msg = packet("W1AW", "WIDE1-1", ":N0CALL   :hello there{01");
    aprsTraffic_record(&msg, APRS_TRAFFIC_FOR_ME, 150);
    aprsTraffic_record(&msg, APRS_TRAFFIC_FOR_ME, 159);
    CHECK((aprsTraffic_get(0)->flags & APRS_TRAFFIC_DUP) != 0);

    /* Hearing our own transmission digipeated is not a duplicate of it. */
    aprsTraffic_record(&direct, APRS_TRAFFIC_TX, 200);
    aprsTraffic_record(&digi, 0, 202);
    CHECK((aprsTraffic_get(0)->flags & APRS_TRAFFIC_DUP) == 0);
}

TEST_CASE("aprs traffic: a long line is cut to fit", "[aprs][traffic]")
{
    aprsTraffic_init();

    std::string info(110, 'x');
    aprsPacket p = packet("N2BP-7", nullptr, info.c_str());
    aprsTraffic_record(&p, 0, 1);

    CHECK(strlen(aprsTraffic_get(0)->line) == APRS_TRAFFIC_LINE_LEN - 1);
}
