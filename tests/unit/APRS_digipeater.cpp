/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The digipeater against the APRS Foundation's worked examples and the
 * cases aprx defends against: the frame transform alone, then the policy
 * with its duplicate window and viscous hold.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include <vector>

#include "protocols/APRS/digipeater.h"
#include "protocols/APRS/packet.h"

typedef std::vector<uint8_t> bytes;

static bytes frame(const char *src, const char *path, const char *info,
                   const char *dest = "APZ")
{
    bytes out(APRS_PACLEN);
    ssize_t n = aprsFrame_build(out.data(), out.size(), dest, src, path, info,
                                strlen(info));
    REQUIRE(n > 0);
    out.resize(n);
    return out;
}

/* "SRC>DEST,PATH:info" of a frame, every used hop starred. */
static std::string tnc2(const bytes &f)
{
    struct aprsPacket pkt;
    REQUIRE(aprsPkt_fromFrame(f.data(), f.size(), &pkt) == 0);
    char src[APRS_ADDR_STR_LEN], dst[APRS_ADDR_STR_LEN], path[80];
    aprsAddr_toStr(&pkt.addresses[1], src, sizeof(src));
    aprsAddr_toStr(&pkt.addresses[0], dst, sizeof(dst));
    aprsPath_toStr(&pkt, path, sizeof(path));
    std::string out = std::string(src) + ">" + dst;
    if (path[0] != '\0')
        out += std::string(",") + path;
    return out + ":" + std::string(pkt.info, pkt.infoLen);
}

static std::string transformed(const bytes &in, const char *me,
                               const char *alias, uint8_t maxN)
{
    bytes out(APRS_PACLEN);
    size_t n = ax25DigiTransform(in.data(), in.size(), out.data(), out.size(),
                                 me, alias, maxN);
    if (n == 0)
        return "";
    out.resize(n);
    return tnc2(out);
}

TEST_CASE("digipeater: the three rewrites of the algorithm", "[aprs][digi]")
{
    const char *me = "KD2ABC-7";

    /* 4.3.1: our callsign or alias is replaced by our callsign, used */
    CHECK(transformed(frame("WB2OSZ", "EOC-1", "something"), me, "EOC-1", 1)
          == "WB2OSZ>APZ,KD2ABC-7*:something");
    CHECK(transformed(frame("WB2OSZ", "KD2ABC-7", "x"), me, nullptr, 1)
          == "WB2OSZ>APZ,KD2ABC-7*:x");

    /* 4.3.2a: N >= 2 is decremented, our callsign inserted before it */
    CHECK(transformed(frame("W9XYZ", "WIDE2-2", "something"), me, "", 2)
          == "W9XYZ>APZ,KD2ABC-7*,WIDE2-1:something");

    /* 4.3.2b: the last hop is spent, the alias replaced outright */
    CHECK(transformed(frame("W9XYZ", "WIDE2-1", "something"), me, "", 2)
          == "W9XYZ>APZ,KD2ABC-7*:something");

    /* The usual mobile path: WIDE1-1 served, WIDE2-1 left for the next */
    CHECK(transformed(frame("W9XYZ", "WIDE1-1,WIDE2-1", "pos"), me, "", 1)
          == "W9XYZ>APZ,KD2ABC-7*,WIDE2-1:pos");

    /* The first UNUSED via is judged, not the first one */
    CHECK(transformed(frame("WB2OSZ", "N2GH*,WIDE2-1", "s"), me, "", 2)
          == "WB2OSZ>APZ,N2GH*,KD2ABC-7*:s");
}

TEST_CASE("digipeater: frames that are not ours to repeat", "[aprs][digi]")
{
    const char *me = "KD2ABC-7";

    /* No via, every via used, or none addressed to us */
    CHECK(transformed(frame("W9XYZ", nullptr, "s"), me, "", 2) == "");
    CHECK(transformed(frame("W9XYZ", "N2GH*,W2UB*", "s"), me, "", 2) == "");
    CHECK(transformed(frame("W9XYZ", "W1AW-1,WIDE2-1", "s"), me, "", 2) == "");

    /* A generic alias above the n we serve: WIDE3-3 is not a fill-in's */
    CHECK(transformed(frame("W9XYZ", "WIDE3-3", "s"), me, "", 1) == "");
    CHECK(transformed(frame("W9XYZ", "WIDE3-3", "s"), me, "", 4)
          == "W9XYZ>APZ,KD2ABC-7*,WIDE3-2:s");

    /* Malformed generic: N above n, or N of zero */
    CHECK(transformed(frame("W9XYZ", "WIDE1-3", "s"), me, "", 4) == "");
    CHECK(transformed(frame("W9XYZ", "WIDE2", "s"), me, "", 4) == "");

    /* No room to insert once the path holds eight addresses */
    CHECK(transformed(frame("W9XYZ", "A*,B*,C*,D*,E*,F*,G*,WIDE2-2", "s"), me,
                      "", 4)
          == "");
    /* ...but replacing still fits */
    CHECK(transformed(frame("W9XYZ", "A*,B*,C*,D*,E*,F*,G*,WIDE2-1", "s"), me,
                      "", 4)
          == "W9XYZ>APZ,A*,B*,C*,D*,E*,F*,G*,KD2ABC-7*:s");

    /* The info field is never touched, whatever it holds */
    bytes in = frame("W9XYZ", "WIDE1-1", ":KD2ABC-7 :hello{01");
    bytes out(APRS_PACLEN);
    size_t n = ax25DigiTransform(in.data(), in.size(), out.data(), out.size(),
                                 me, "", 1);
    REQUIRE(n == in.size());
    CHECK(memcmp(&out[3 * 7], &in[3 * 7], in.size() - 3 * 7) == 0);

    /* In place works too */
    bytes same = in;
    n = ax25DigiTransform(same.data(), same.size(), same.data(), same.size(),
                          me, "", 1);
    REQUIRE(n == in.size());
    same.resize(n);
    CHECK(tnc2(same) == "W9XYZ>APZ,KD2ABC-7*::KD2ABC-7 :hello{01");
}

static struct aprsDigiConfig fillin()
{
    struct aprsDigiConfig cfg = {};
    cfg.mode = APRS_DIGI_FILLIN;
    cfg.delay = 5;
    cfg.maxHops = 4;
    cfg.directOnly = true;
    strcpy(cfg.me, "KD2ABC-7");
    return cfg;
}

static std::string due(struct aprsDigi &d, uint32_t now)
{
    bytes out(APRS_PACLEN);
    size_t n = aprsDigi_due(&d, now, out.data(), out.size());
    if (n == 0)
        return "";
    out.resize(n);
    return tnc2(out);
}

TEST_CASE("digipeater: a fill-in holds a frame and sends it if nobody else did",
          "[aprs][digi]")
{
    static struct aprsDigi d;
    struct aprsDigiConfig cfg = fillin();
    aprsDigi_init(&d, &cfg);

    bytes f = frame("W9XYZ", "WIDE1-1,WIDE2-1", "!4903.50N/07201.75W-");
    CHECK(aprsDigi_offer(&d, f.data(), f.size(), 100) == APRS_DIGI_HELD);
    CHECK(d.stats.heard == 1);
    CHECK(due(d, 104) == "");
    CHECK(due(d, 105) == "W9XYZ>APZ,KD2ABC-7*,WIDE2-1:!4903.50N/07201.75W-");
    CHECK(d.stats.repeated == 1);
    CHECK(due(d, 106) == "");

    /* Heard again within the window, directly or via someone: nothing */
    CHECK(aprsDigi_offer(&d, f.data(), f.size(), 110) == APRS_DIGI_DUPLICATE);
    bytes via = frame("W9XYZ", "W2UB*,WIDE2-1", "!4903.50N/07201.75W-");
    CHECK(aprsDigi_offer(&d, via.data(), via.size(), 111)
          == APRS_DIGI_DUPLICATE);
    CHECK(due(d, 120) == "");

    /* After the window it is a new packet */
    CHECK(aprsDigi_offer(&d, f.data(), f.size(), 140) == APRS_DIGI_HELD);
    CHECK(d.stats.heard == 2);
}

TEST_CASE("digipeater: standing down when another station repeats first",
          "[aprs][digi]")
{
    static struct aprsDigi d;
    struct aprsDigiConfig cfg = fillin();
    aprsDigi_init(&d, &cfg);

    bytes f = frame("W9XYZ", "WIDE1-1,WIDE2-1", ">status");
    CHECK(aprsDigi_offer(&d, f.data(), f.size(), 200) == APRS_DIGI_HELD);

    /* The same packet comes back carried by W2UB: ours is not needed */
    bytes via = frame("W9XYZ", "W2UB*,WIDE2-1", ">status");
    CHECK(aprsDigi_offer(&d, via.data(), via.size(), 202)
          == APRS_DIGI_STOOD_DOWN);
    CHECK(due(d, 210) == "");
    CHECK(d.stats.stoodDown == 1);
    CHECK(d.stats.repeated == 0);

    /* A path that changed the destination SSID is still the same packet */
    bytes g = frame("W9XYZ", "WIDE1-1", ">again", "APZ-3");
    CHECK(aprsDigi_offer(&d, g.data(), g.size(), 300) == APRS_DIGI_HELD);
    bytes g2 = frame("W9XYZ", "W2UB*", ">again", "APZ");
    CHECK(aprsDigi_offer(&d, g2.data(), g2.size(), 301)
          == APRS_DIGI_STOOD_DOWN);
}

TEST_CASE("digipeater: refusals", "[aprs][digi]")
{
    static struct aprsDigi d;
    struct aprsDigiConfig cfg = fillin();
    aprsDigi_init(&d, &cfg);

    /* Not heard directly: a fill-in leaves it to the wide-area digis */
    bytes carried = frame("W9XYZ", "W2UB*,WIDE1-1", "a");
    CHECK(aprsDigi_offer(&d, carried.data(), carried.size(), 10)
          == APRS_DIGI_REFUSED);
    CHECK(d.stats.refused == 1);

    /* Our own traffic, from either end */
    bytes mine = frame("KD2ABC-7", "WIDE1-1", "b");
    CHECK(aprsDigi_offer(&d, mine.data(), mine.size(), 11)
          == APRS_DIGI_IGNORED);
    bytes loop = frame("W9XYZ", "KD2ABC-7*,WIDE1-1", "c");
    CHECK(aprsDigi_offer(&d, loop.data(), loop.size(), 12)
          == APRS_DIGI_REFUSED);

    /* Not a UI frame: an I frame keeps its own retries */
    bytes ui = frame("W9XYZ", "WIDE1-1", "d");
    bytes iframe = ui;
    iframe[3 * 7] = 0x00;
    CHECK(aprsDigi_offer(&d, iframe.data(), iframe.size(), 13)
          == APRS_DIGI_IGNORED);

    /* A spent path, or one not for us */
    bytes spent = frame("W9XYZ", "N2GH*", "e");
    CHECK(aprsDigi_offer(&d, spent.data(), spent.size(), 14)
          == APRS_DIGI_IGNORED);
    bytes other = frame("W9XYZ", "WIDE2-2", "f");
    CHECK(aprsDigi_offer(&d, other.data(), other.size(), 15)
          == APRS_DIGI_IGNORED);

    /* Switched off: nothing is held */
    cfg.mode = APRS_DIGI_OFF;
    aprsDigi_configure(&d, &cfg);
    CHECK(aprsDigi_offer(&d, ui.data(), ui.size(), 16) == APRS_DIGI_IGNORED);
    CHECK(due(d, 100) == "");
}

TEST_CASE("digipeater: custom mode serves WIDEn-N up to the hop limit",
          "[aprs][digi]")
{
    static struct aprsDigi d;
    struct aprsDigiConfig cfg = fillin();
    cfg.mode = APRS_DIGI_CUSTOM;
    cfg.delay = 0;
    cfg.maxHops = 4;
    cfg.directOnly = false;
    strcpy(cfg.alias, "EOC-1");
    aprsDigi_init(&d, &cfg);

    bytes f = frame("W9XYZ", "WIDE2-2", "g");
    CHECK(aprsDigi_offer(&d, f.data(), f.size(), 50) == APRS_DIGI_HELD);
    CHECK(due(d, 50) == "W9XYZ>APZ,KD2ABC-7*,WIDE2-1:g");

    /* Carried traffic is fine when not direct-only */
    bytes carried = frame("W9XYZ", "W2UB*,WIDE2-1", "h");
    CHECK(aprsDigi_offer(&d, carried.data(), carried.size(), 51)
          == APRS_DIGI_HELD);
    CHECK(due(d, 51) == "W9XYZ>APZ,W2UB*,KD2ABC-7*:h");

    /* The alias answers too */
    bytes alias = frame("WB2OSZ", "EOC-1", "i");
    CHECK(aprsDigi_offer(&d, alias.data(), alias.size(), 52) == APRS_DIGI_HELD);
    CHECK(due(d, 52) == "WB2OSZ>APZ,KD2ABC-7*:i");

    /* Too many hops asked for, or already taken */
    bytes wide7 = frame("W9XYZ", "WIDE7-7", "j");
    CHECK(aprsDigi_offer(&d, wide7.data(), wide7.size(), 53)
          == APRS_DIGI_IGNORED);
    bytes tired = frame("W9XYZ", "A*,B*,C*,D*,WIDE2-1", "k");
    CHECK(aprsDigi_offer(&d, tired.data(), tired.size(), 54)
          == APRS_DIGI_REFUSED);

    /* Only so many frames can be held at once */
    cfg.delay = 9;
    aprsDigi_configure(&d, &cfg);
    for (int i = 0; i < APRS_DIGI_PENDING; i++) {
        std::string info = "n" + std::to_string(i);
        bytes x = frame("W9XYZ", "WIDE1-1", info.c_str());
        CHECK(aprsDigi_offer(&d, x.data(), x.size(), 60) == APRS_DIGI_HELD);
    }
    bytes extra = frame("W9XYZ", "WIDE1-1", "one too many");
    CHECK(aprsDigi_offer(&d, extra.data(), extra.size(), 60)
          == APRS_DIGI_REFUSED);

    /* A reconfiguration drops what was held */
    aprsDigi_configure(&d, &cfg);
    CHECK(due(d, 100) == "");
}
