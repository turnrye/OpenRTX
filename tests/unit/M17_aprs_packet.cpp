/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include <vector>

#include "protocols/M17/AprsPacket.h"
#include "protocols/M17/m17.h"
#include "protocols/APRS/packet.h"

typedef std::vector<uint8_t> bytes;

static bytes ax25(const char *src, const char *path, const char *info)
{
    bytes f(APRS_PACLEN);
    ssize_t n = aprsFrame_build(f.data(), f.size(), "APORT1", src, path, info,
                                strlen(info));
    REQUIRE(n > 0);
    f.resize(n);
    return f;
}

TEST_CASE("M17 APRS: a frame goes out as 0x02, a TNC2 line and a NUL",
          "[m17][aprs]")
{
    bytes frame = ax25("W1AW-9", nullptr, "!4903.50N/07201.75W-");
    uint8_t payload[M17_MAX_PACKET_DATA];

    size_t n = m17_aprsFormat(frame.data(), frame.size(), payload,
                              sizeof(payload));
    const std::string line = "W1AW-9>APORT1:!4903.50N/07201.75W-";
    REQUIRE(n == 1 + line.size() + 1);
    CHECK(payload[0] == M17_PACKET_APRS);
    CHECK(std::string(reinterpret_cast<char *>(payload + 1)) == line);
    CHECK(payload[n - 1] == 0);

    /* And comes back as the same frame */
    uint8_t back[APRS_PACLEN];
    size_t m = m17_aprsFrame(payload, n, back, sizeof(back));
    REQUIRE(m == frame.size());
    CHECK(memcmp(back, frame.data(), m) == 0);

    /* Too long for the caller's buffer */
    CHECK(m17_aprsFormat(frame.data(), frame.size(), payload, 10) == 0);
    CHECK(m17_aprsFrame(payload, n, back, 5) == 0);
}

TEST_CASE("M17 APRS: a received line becomes an AX.25 frame", "[m17][aprs]")
{
    const char *line = "N2BP-7>APRS,WIDE1-1::K1ABC    :hi{01";
    bytes payload = { M17_PACKET_APRS };
    payload.insert(payload.end(), line, line + strlen(line));

    /* With or without its NUL */
    uint8_t frame[APRS_PACLEN];
    size_t n = m17_aprsFrame(payload.data(), payload.size(), frame,
                             sizeof(frame));
    REQUIRE(n > 0);
    payload.push_back(0);
    CHECK(m17_aprsFrame(payload.data(), payload.size(), frame, sizeof(frame))
          == n);

    struct aprsPacket pkt;
    REQUIRE(aprsPkt_fromFrame(frame, n, &pkt) == 0);
    char src[APRS_ADDR_STR_LEN], path[40];
    aprsAddr_toStr(&pkt.addresses[1], src, sizeof(src));
    aprsPath_toStr(&pkt, path, sizeof(path));
    CHECK(std::string(src) == "N2BP-7");
    CHECK(std::string(path) == "WIDE1-1");
    CHECK(std::string(pkt.info, pkt.infoLen) == ":K1ABC    :hi{01");
}

TEST_CASE("M17 APRS: other packets are not APRS", "[m17][aprs]")
{
    uint8_t frame[APRS_PACLEN];

    const uint8_t sms[] = { 0x05, 'h', 'i', 0x00 };
    CHECK(m17_aprsFrame(sms, sizeof(sms), frame, sizeof(frame)) == 0);

    /* Raw AX.25 is 0x01's, conventional packet radio, not APRS */
    bytes ax = ax25("W1AW-9", nullptr, ">status");
    bytes raw = { 0x01 };
    raw.insert(raw.end(), ax.begin(), ax.end());
    CHECK(m17_aprsFrame(raw.data(), raw.size(), frame, sizeof(frame)) == 0);

    const uint8_t empty[] = { M17_PACKET_APRS };
    CHECK(m17_aprsFrame(empty, sizeof(empty), frame, sizeof(frame)) == 0);

    /* A source AX.25 cannot carry, as APRS-IS has */
    const char *winlink =
        "WINLINK>APWL2K:;KD7ZYN-10*111313z4724.83NW12219.11Wa";
    bytes payload = { M17_PACKET_APRS };
    payload.insert(payload.end(), winlink, winlink + strlen(winlink));
    CHECK(m17_aprsFrame(payload.data(), payload.size(), frame, sizeof(frame))
          == 0);
}
