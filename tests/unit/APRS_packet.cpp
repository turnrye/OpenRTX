/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Contract tests for the AX.25 -> aprsPacket parser. Frames are synthesised
 * here rather than demodulated so each test can pin one property of the
 * parser exactly: address unshifting, SSIDs, digipeater paths, DTI
 * classification, addressed-message unwrapping, and the malformed frames that
 * must be rejected instead of parsed into garbage.
 */

#include <catch2/catch_test_macros.hpp>

#include "protocols/APRS/packet.h"
#include <cstring>

namespace
{

/** A frame the way the decoder hands one back: raw bytes and a length. */
struct frameData {
    uint8_t data[APRS_PACLEN];
    uint8_t len;
};

/**
 * Write one shifted-ASCII AX.25 address into a frame.
 *
 * The flags byte is CRRSSSSL: command/response, two reserved bits that are
 * transmitted as ones, the four-bit SSID, and the last-address marker.
 */
void putAddress(uint8_t *dst, const char *call, uint8_t ssid, bool repeated,
                bool last)
{
    size_t i = 0;

    for (; (i < 6) && (call[i] != '\0'); i++)
        dst[i] = (uint8_t)(call[i] << 1);
    for (; i < 6; i++)
        dst[i] = (uint8_t)(' ' << 1);

    dst[6] = 0x60; /* reserved bits */
    if (repeated)
        dst[6] |= 0x80;
    dst[6] |= (uint8_t)((ssid & 0x0f) << 1);
    if (last)
        dst[6] |= 0x01;
}

/**
 * Build a two-address (destination, source) UI frame carrying @p info.
 */
frameData makeFrame(const char *dst, uint8_t dstSsid, const char *src,
                    uint8_t srcSsid, const char *info)
{
    frameData frame;
    memset(&frame, 0, sizeof(frame));

    putAddress(frame.data + 0, dst, dstSsid, false, false);
    putAddress(frame.data + 7, src, srcSsid, false, true);
    frame.data[14] = 0x03; /* UI frame           */
    frame.data[15] = 0xf0; /* no layer 3 protocol */

    const size_t infoLen = strlen(info);
    memcpy(frame.data + 16, info, infoLen);
    frame.len = (uint8_t)(16 + infoLen);

    return frame;
}

} // namespace

TEST_CASE("APRS packet: destination and source are unshifted", "[aprs]")
{
    frameData frame = makeFrame("APRS", 0, "N2BP", 7, ">hello");
    aprsPacket pkt;

    REQUIRE(aprsPktFromFrame(frame.data, frame.len, &pkt) == true);
    REQUIRE(pkt.addressesLen == 2);
    REQUIRE(strcmp(pkt.addresses[0].addr, "APRS") == 0);
    REQUIRE(pkt.addresses[0].ssid == 0);
    REQUIRE(strcmp(pkt.addresses[1].addr, "N2BP") == 0);
    REQUIRE(pkt.addresses[1].ssid == 7);
    REQUIRE(strcmp(pkt.info, ">hello") == 0);
    REQUIRE(pkt.infoLen == 6);
}

TEST_CASE("APRS packet: a single address can be read without a full parse",
          "[aprs]")
{
    frameData frame;
    memset(&frame, 0, sizeof(frame));

    putAddress(frame.data + 0, "APRS", 0, false, false);
    putAddress(frame.data + 7, "N2BP", 7, false, false);
    putAddress(frame.data + 14, "WIDE1", 1, true, true);
    frame.data[21] = 0x03;
    frame.data[22] = 0xf0;
    memcpy(frame.data + 23, ">hi", 3);
    frame.len = 26;

    aprsAddress addr;

    REQUIRE(aprsAddrFromFrame(frame.data, frame.len, 0, &addr) == true);
    REQUIRE(strcmp(addr.addr, "APRS") == 0);

    REQUIRE(aprsAddrFromFrame(frame.data, frame.len, 1, &addr) == true);
    REQUIRE(strcmp(addr.addr, "N2BP") == 0);
    REQUIRE(addr.ssid == 7);
    REQUIRE(addr.repeated == 0);

    REQUIRE(aprsAddrFromFrame(frame.data, frame.len, 2, &addr) == true);
    REQUIRE(strcmp(addr.addr, "WIDE1") == 0);
    REQUIRE(addr.ssid == 1);
    REQUIRE(addr.repeated == 1);

    /* Past the end of the address field, and on a frame that has none. */
    REQUIRE(aprsAddrFromFrame(frame.data, frame.len, 3, &addr) == false);

    frameData tooShort;
    memset(&tooShort, 0, sizeof(tooShort));
    putAddress(tooShort.data, "APRS", 0, false, true);
    tooShort.len = 7;
    REQUIRE(aprsAddrFromFrame(tooShort.data, tooShort.len, 0, &addr) == false);

    REQUIRE(aprsAddrFromFrame(nullptr, 20, 0, &addr) == false);
    REQUIRE(aprsAddrFromFrame(frame.data, frame.len, 0, nullptr) == false);
}

TEST_CASE("APRS packet: address formatting follows the TNC2 convention",
          "[aprs]")
{
    frameData frame = makeFrame("APRS", 0, "N2BP", 7, ">hi");
    aprsPacket pkt;
    char buf[APRS_ADDR_STR_LEN];

    REQUIRE(aprsPktFromFrame(frame.data, frame.len, &pkt) == true);

    /* SSID 0 is left off entirely. */
    REQUIRE(aprsAddrToStr(&pkt.addresses[0], buf, sizeof(buf)) == 4);
    REQUIRE(strcmp(buf, "APRS") == 0);

    REQUIRE(aprsAddrToStr(&pkt.addresses[1], buf, sizeof(buf)) == 6);
    REQUIRE(strcmp(buf, "N2BP-7") == 0);
}

TEST_CASE("APRS packet: the longest callsign-SSID pair still fits", "[aprs]")
{
    frameData frame = makeFrame("APRS", 0, "ABCDEF", 15, ">hi");
    aprsPacket pkt;
    char buf[APRS_ADDR_STR_LEN];

    REQUIRE(aprsPktFromFrame(frame.data, frame.len, &pkt) == true);
    REQUIRE(aprsAddrToStr(&pkt.addresses[1], buf, sizeof(buf)) == 9);
    REQUIRE(strcmp(buf, "ABCDEF-15") == 0);
}

TEST_CASE("APRS packet: digipeater path is formatted with repeat markers",
          "[aprs]")
{
    frameData frame;
    memset(&frame, 0, sizeof(frame));

    putAddress(frame.data + 0, "APRS", 0, false, false);
    putAddress(frame.data + 7, "N2BP", 7, false, false);
    putAddress(frame.data + 14, "WIDE1", 1, true, false);
    putAddress(frame.data + 21, "WIDE2", 2, false, true);
    frame.data[28] = 0x03;
    frame.data[29] = 0xf0;
    memcpy(frame.data + 30, ">hi", 3);
    frame.len = 33;

    aprsPacket pkt;
    REQUIRE(aprsPktFromFrame(frame.data, frame.len, &pkt) == true);
    REQUIRE(pkt.addressesLen == 4);

    char path[32];
    REQUIRE(aprsPathToStr(&pkt, path, sizeof(path)) == strlen(path));
    REQUIRE(strcmp(path, "WIDE1-1*,WIDE2-2") == 0);
}

TEST_CASE("APRS packet: a directly heard packet has an empty path", "[aprs]")
{
    frameData frame = makeFrame("APRS", 0, "N2BP", 7, ">hi");
    aprsPacket pkt;
    char path[32];

    REQUIRE(aprsPktFromFrame(frame.data, frame.len, &pkt) == true);
    REQUIRE(aprsPathToStr(&pkt, path, sizeof(path)) == 0);
    REQUIRE(path[0] == '\0');
}

TEST_CASE("APRS packet: the data type identifier is classified", "[aprs]")
{
    struct {
        const char *info;
        enum aprsType type;
    } cases[] = {
        { ":N2BP-7   :hi", APRS_TYPE_MESSAGE },
        { "!4903.50N/07201.75W-", APRS_TYPE_POSITION },
        { "=4903.50N/07201.75W-", APRS_TYPE_POSITION },
        { "/092345z4903.50N/07201.75W>", APRS_TYPE_POSITION },
        { "@092345z4903.50N/07201.75W>", APRS_TYPE_POSITION },
        { ">on the air", APRS_TYPE_STATUS },
        { ";LEADER   *092345z4903.50N/07201.75W>", APRS_TYPE_OBJECT },
        { ")AID #2!4903.50N/07201.75W", APRS_TYPE_OBJECT },
        { "T#005,199,000,255,073,123,01101001", APRS_TYPE_TELEMETRY },
        { "?APRS?", APRS_TYPE_OTHER },
    };

    for (const auto &c : cases) {
        frameData frame = makeFrame("APRS", 0, "N2BP", 7, c.info);
        aprsPacket pkt;

        INFO("info field: " << c.info);
        REQUIRE(aprsPktFromFrame(frame.data, frame.len, &pkt) == true);
        REQUIRE(pkt.type == c.type);
    }
}

TEST_CASE("APRS packet: an addressed message is split into addressee and text",
          "[aprs]")
{
    frameData frame = makeFrame("APRS", 0, "N2BP", 7, ":W1AW     :hello there");
    aprsPacket pkt;
    char addressee[APRS_ADDR_STR_LEN];
    char text[APRS_PACLEN];

    REQUIRE(aprsPktFromFrame(frame.data, frame.len, &pkt) == true);
    REQUIRE(pkt.type == APRS_TYPE_MESSAGE);
    REQUIRE(
        aprsMsgUnwrap(&pkt, addressee, sizeof(addressee), text, sizeof(text))
        == true);
    REQUIRE(strcmp(addressee, "W1AW") == 0);
    REQUIRE(strcmp(text, "hello there") == 0);
}

TEST_CASE("APRS packet: the acknowledgement sequence is stripped", "[aprs]")
{
    frameData frame = makeFrame("APRS", 0, "N2BP", 7, ":W1AW     :ping{042");
    aprsPacket pkt;
    char addressee[APRS_ADDR_STR_LEN];
    char text[APRS_PACLEN];

    REQUIRE(aprsPktFromFrame(frame.data, frame.len, &pkt) == true);
    REQUIRE(
        aprsMsgUnwrap(&pkt, addressee, sizeof(addressee), text, sizeof(text))
        == true);
    REQUIRE(strcmp(addressee, "W1AW") == 0);
    REQUIRE(strcmp(text, "ping") == 0);
}

TEST_CASE("APRS packet: a full-width addressee keeps every character", "[aprs]")
{
    frameData frame = makeFrame("APRS", 0, "N2BP", 7, ":ABCDEF-15:x");
    aprsPacket pkt;
    char addressee[APRS_ADDR_STR_LEN];
    char text[APRS_PACLEN];

    REQUIRE(aprsPktFromFrame(frame.data, frame.len, &pkt) == true);
    REQUIRE(
        aprsMsgUnwrap(&pkt, addressee, sizeof(addressee), text, sizeof(text))
        == true);
    REQUIRE(strcmp(addressee, "ABCDEF-15") == 0);
    REQUIRE(strcmp(text, "x") == 0);
}

TEST_CASE("APRS packet: an empty message body unwraps to an empty string",
          "[aprs]")
{
    frameData frame = makeFrame("APRS", 0, "N2BP", 7, ":W1AW     :");
    aprsPacket pkt;
    char addressee[APRS_ADDR_STR_LEN];
    char text[APRS_PACLEN];

    REQUIRE(aprsPktFromFrame(frame.data, frame.len, &pkt) == true);
    REQUIRE(
        aprsMsgUnwrap(&pkt, addressee, sizeof(addressee), text, sizeof(text))
        == true);
    REQUIRE(strcmp(addressee, "W1AW") == 0);
    REQUIRE(text[0] == '\0');
}

TEST_CASE("APRS packet: unwrapping rejects info fields that only look like "
          "messages",
          "[aprs]")
{
    aprsPacket pkt;
    char addressee[APRS_ADDR_STR_LEN];
    char text[APRS_PACLEN];

    SECTION("no separator at the fixed offset")
    {
        frameData frame = makeFrame("APRS", 0, "N2BP", 7, ":not a message");
        REQUIRE(aprsPktFromFrame(frame.data, frame.len, &pkt) == true);
        REQUIRE(pkt.type == APRS_TYPE_MESSAGE);
        REQUIRE(aprsMsgUnwrap(&pkt, addressee, sizeof(addressee), text,
                              sizeof(text))
                == false);
    }

    SECTION("too short to hold an addressee")
    {
        frameData frame = makeFrame("APRS", 0, "N2BP", 7, ":W1AW");
        REQUIRE(aprsPktFromFrame(frame.data, frame.len, &pkt) == true);
        REQUIRE(aprsMsgUnwrap(&pkt, addressee, sizeof(addressee), text,
                              sizeof(text))
                == false);
    }

    SECTION("blank addressee")
    {
        frameData frame = makeFrame("APRS", 0, "N2BP", 7, ":         :hi");
        REQUIRE(aprsPktFromFrame(frame.data, frame.len, &pkt) == true);
        REQUIRE(aprsMsgUnwrap(&pkt, addressee, sizeof(addressee), text,
                              sizeof(text))
                == false);
    }

    SECTION("not a message at all")
    {
        frameData frame = makeFrame("APRS", 0, "N2BP", 7, ">status text");
        REQUIRE(aprsPktFromFrame(frame.data, frame.len, &pkt) == true);
        REQUIRE(aprsMsgUnwrap(&pkt, addressee, sizeof(addressee), text,
                              sizeof(text))
                == false);
    }
}

TEST_CASE("APRS packet: malformed frames are rejected", "[aprs]")
{
    aprsPacket pkt;

    SECTION("too short for two addresses")
    {
        frameData frame;
        memset(&frame, 0, sizeof(frame));
        putAddress(frame.data, "APRS", 0, false, true);
        frame.len = 7;
        REQUIRE(aprsPktFromFrame(frame.data, frame.len, &pkt) == false);
    }

    SECTION("address field is never terminated")
    {
        frameData frame;
        memset(&frame, 0, sizeof(frame));
        for (uint8_t i = 0; i < 4; i++)
            putAddress(frame.data + i * 7, "NOEND", 0, false, false);
        frame.len = 30;
        REQUIRE(aprsPktFromFrame(frame.data, frame.len, &pkt) == false);
    }

    SECTION("more addresses than AX.25 allows")
    {
        frameData frame;
        memset(&frame, 0, sizeof(frame));
        for (uint8_t i = 0; i < APRS_MAX_ADDRESSES + 1; i++)
            putAddress(frame.data + i * 7, "RELAY", 0, false, false);
        frame.len = (uint8_t)(7 * (APRS_MAX_ADDRESSES + 1));
        REQUIRE(aprsPktFromFrame(frame.data, frame.len, &pkt) == false);
    }

    SECTION("no room left for an info field")
    {
        frameData frame = makeFrame("APRS", 0, "N2BP", 7, "");
        REQUIRE(frame.len == 16);
        REQUIRE(aprsPktFromFrame(frame.data, frame.len, &pkt) == false);
    }

    SECTION("null arguments")
    {
        frameData frame = makeFrame("APRS", 0, "N2BP", 7, ">hi");
        REQUIRE(aprsPktFromFrame(nullptr, 20, &pkt) == false);
        REQUIRE(aprsPktFromFrame(frame.data, frame.len, nullptr) == false);
    }
}

/* ------------------------------------------------------------------------ *
 * Frame construction.
 *
 * The builder is checked two ways.  Round-tripping through the parser above
 * proves the two agree with each other, which is what the radio's own RX and
 * TX paths need.  Agreeing with each other is not the same as agreeing with
 * the rest of the world, though, so the byte-for-byte vector below comes from
 * scripts/aprs_gen_baseband.py — an independent implementation written
 * against the AX.25 specification, whose output is also what the RX e2e test
 * feeds to the demodulator.
 * ------------------------------------------------------------------------ */

TEST_CASE("APRS address: TNC2 text parses back into an address", "[aprs]")
{
    aprsAddress addr;

    SECTION("callsign without an SSID")
    {
        REQUIRE(aprsAddrFromStr("W1AW", &addr) == true);
        REQUIRE(strcmp(addr.addr, "W1AW") == 0);
        REQUIRE(addr.ssid == 0);
    }

    SECTION("callsign with an SSID")
    {
        REQUIRE(aprsAddrFromStr("N0CALL-15", &addr) == true);
        REQUIRE(strcmp(addr.addr, "N0CALL") == 0);
        REQUIRE(addr.ssid == 15);
    }

    SECTION("lower case is folded up so a typed recipient still works")
    {
        REQUIRE(aprsAddrFromStr("n0call-7", &addr) == true);
        REQUIRE(strcmp(addr.addr, "N0CALL") == 0);
        REQUIRE(addr.ssid == 7);
    }

    SECTION("round trip through the formatter")
    {
        char out[APRS_ADDR_STR_LEN];
        REQUIRE(aprsAddrFromStr("N0CALL-7", &addr) == true);
        REQUIRE(aprsAddrToStr(&addr, out, sizeof(out)) == 8);
        REQUIRE(strcmp(out, "N0CALL-7") == 0);
    }

    SECTION("malformed text is rejected")
    {
        REQUIRE(aprsAddrFromStr("", &addr) == false);
        REQUIRE(aprsAddrFromStr("-7", &addr) == false);
        REQUIRE(aprsAddrFromStr("N0CALL-", &addr) == false);
        REQUIRE(aprsAddrFromStr("N0CALL-16", &addr) == false);
        REQUIRE(aprsAddrFromStr("N0CALL-x", &addr) == false);
        REQUIRE(aprsAddrFromStr("TOOLONGCALL", &addr) == false);
        REQUIRE(aprsAddrFromStr("WITH SPACE", &addr) == false);
        REQUIRE(aprsAddrFromStr(nullptr, &addr) == false);
        REQUIRE(aprsAddrFromStr("W1AW", nullptr) == false);
    }
}

TEST_CASE("APRS frame builder: a built frame parses back to what went in",
          "[aprs]")
{
    frameData frame;
    aprsPacket pkt;
    char addr[APRS_ADDR_STR_LEN];
    char path[32];

    const char info[] = ":W1AW     :hello";

    REQUIRE((frame.len = (uint8_t)aprsFrameBuild(
                 frame.data, sizeof(frame.data), APRS_TOCALL, "N0CALL-7",
                 APRS_DEFAULT_PATH, info, strlen(info)))
            > 0);
    REQUIRE(aprsPktFromFrame(frame.data, frame.len, &pkt) == true);

    REQUIRE(pkt.addressesLen == 4);

    aprsAddrToStr(&pkt.addresses[0], addr, sizeof(addr));
    REQUIRE(strcmp(addr, APRS_TOCALL) == 0);

    aprsAddrToStr(&pkt.addresses[1], addr, sizeof(addr));
    REQUIRE(strcmp(addr, "N0CALL-7") == 0);

    aprsPathToStr(&pkt, path, sizeof(path));
    REQUIRE(strcmp(path, APRS_DEFAULT_PATH) == 0);

    REQUIRE(pkt.type == APRS_TYPE_MESSAGE);
    REQUIRE(strcmp(pkt.info, info) == 0);
}

TEST_CASE("APRS frame builder: the frame matches an independent encoder",
          "[aprs]")
{
    /* scripts/aprs_gen_baseband.py, parse_tnc2() of
     *   N0CALL-7>APORT1,WIDE1-1,WIDE2-1::W1AW     :hello
     * Regenerate with:
     *   python3 -c "import importlib.util as u; \
     *     s=u.spec_from_file_location('g','scripts/aprs_gen_baseband.py'); \
     *     m=u.module_from_spec(s); s.loader.exec_module(m); \
     *     print(m.parse_tnc2('N0CALL-7>APORT1,WIDE1-1,WIDE2-1::W1AW     :hello').hex())"
     */
    static const uint8_t expected[] = {
        0x82, 0xa0, 0x9e, 0xa4, 0xa8, 0x62, 0xe0, 0x9c, 0x60, 0x86, 0x82, 0x98,
        0x98, 0x6e, 0xae, 0x92, 0x88, 0x8a, 0x62, 0x40, 0x62, 0xae, 0x92, 0x88,
        0x8a, 0x64, 0x40, 0x63, 0x03, 0xf0, 0x3a, 0x57, 0x31, 0x41, 0x57, 0x20,
        0x20, 0x20, 0x20, 0x20, 0x3a, 0x68, 0x65, 0x6c, 0x6c, 0x6f
    };

    frameData frame;
    const char info[] = ":W1AW     :hello";

    REQUIRE((frame.len = (uint8_t)aprsFrameBuild(
                 frame.data, sizeof(frame.data), APRS_TOCALL, "N0CALL-7",
                 APRS_DEFAULT_PATH, info, strlen(info)))
            > 0);

    REQUIRE(frame.len == sizeof(expected));
    REQUIRE(memcmp(frame.data, expected, sizeof(expected)) == 0);
}

TEST_CASE("APRS frame builder: a path is optional", "[aprs]")
{
    frameData frame;
    aprsPacket pkt;
    char path[32];

    const char info[] = ">heard direct";

    REQUIRE((frame.len = (uint8_t)aprsFrameBuild(frame.data, sizeof(frame.data),
                                                 APRS_TOCALL, "N0CALL", nullptr,
                                                 info, strlen(info)))
            > 0);
    REQUIRE(aprsPktFromFrame(frame.data, frame.len, &pkt) == true);
    REQUIRE(pkt.addressesLen == 2);
    REQUIRE(aprsPathToStr(&pkt, path, sizeof(path)) == 0);

    REQUIRE((frame.len = (uint8_t)aprsFrameBuild(frame.data, sizeof(frame.data),
                                                 APRS_TOCALL, "N0CALL", "",
                                                 info, strlen(info)))
            > 0);
    REQUIRE(aprsPktFromFrame(frame.data, frame.len, &pkt) == true);
    REQUIRE(pkt.addressesLen == 2);
}

TEST_CASE("APRS frame builder: a repeated path element round-trips", "[aprs]")
{
    frameData frame;
    aprsPacket pkt;
    char path[32];

    const char info[] = ">digipeated";

    REQUIRE((frame.len = (uint8_t)aprsFrameBuild(
                 frame.data, sizeof(frame.data), APRS_TOCALL, "N0CALL",
                 "WIDE1-1*,WIDE2-1", info, strlen(info)))
            > 0);
    REQUIRE(aprsPktFromFrame(frame.data, frame.len, &pkt) == true);
    REQUIRE(aprsPathToStr(&pkt, path, sizeof(path)) > 0);
    REQUIRE(strcmp(path, "WIDE1-1*,WIDE2-1") == 0);
}

TEST_CASE("APRS frame builder: invalid arguments are rejected", "[aprs]")
{
    frameData frame;
    const char info[] = ">hi";

    SECTION("null arguments")
    {
        REQUIRE((aprsFrameBuild(nullptr, APRS_PACLEN, APRS_TOCALL, "N0CALL",
                                nullptr, info, strlen(info)))
                == 0);
        REQUIRE((frame.len = (uint8_t)aprsFrameBuild(
                     frame.data, sizeof(frame.data), nullptr, "N0CALL", nullptr,
                     info, strlen(info)))
                == 0);
        REQUIRE((frame.len = (uint8_t)aprsFrameBuild(
                     frame.data, sizeof(frame.data), APRS_TOCALL, nullptr,
                     nullptr, info, strlen(info)))
                == 0);
        REQUIRE((frame.len = (uint8_t)aprsFrameBuild(
                     frame.data, sizeof(frame.data), APRS_TOCALL, "N0CALL",
                     nullptr, nullptr, strlen(info)))
                == 0);
    }

    SECTION("an empty info field would not parse back")
    {
        REQUIRE((frame.len = (uint8_t)aprsFrameBuild(
                     frame.data, sizeof(frame.data), APRS_TOCALL, "N0CALL",
                     nullptr, info, 0))
                == 0);
    }

    SECTION("a malformed source or path")
    {
        REQUIRE((frame.len = (uint8_t)aprsFrameBuild(
                     frame.data, sizeof(frame.data), APRS_TOCALL, "N0CALL-99",
                     nullptr, info, strlen(info)))
                == 0);
        REQUIRE((frame.len = (uint8_t)aprsFrameBuild(
                     frame.data, sizeof(frame.data), APRS_TOCALL, "N0CALL",
                     "WIDE1-1,", info, strlen(info)))
                == 0);
        REQUIRE((frame.len = (uint8_t)aprsFrameBuild(
                     frame.data, sizeof(frame.data), APRS_TOCALL, "N0CALL",
                     "TOOLONGDIGI", info, strlen(info)))
                == 0);
    }

    SECTION("more digipeaters than AX.25 allows")
    {
        REQUIRE((frame.len = (uint8_t)aprsFrameBuild(
                     frame.data, sizeof(frame.data), APRS_TOCALL, "N0CALL",
                     "W1,W2,W3,W4,W5,W6,W7,W8,W9", info, strlen(info)))
                == 0);
    }

    SECTION("a frame that would not fit")
    {
        char big[APRS_PACLEN];
        memset(big, 'x', sizeof(big));
        REQUIRE((frame.len = (uint8_t)aprsFrameBuild(
                     frame.data, sizeof(frame.data), APRS_TOCALL, "N0CALL",
                     APRS_DEFAULT_PATH, big, sizeof(big)))
                == 0);
    }
}

TEST_CASE("APRS message: a formatted message unwraps to what went in", "[aprs]")
{
    char info[APRS_MSG_ADDRESSEE_LEN + APRS_MSG_TEXT_MAX + 3];
    frameData frame;
    aprsPacket pkt;
    char addressee[APRS_ADDR_STR_LEN];
    char text[APRS_PACLEN];

    SECTION("a short addressee is padded to nine characters")
    {
        REQUIRE(aprsMsgFormat(info, sizeof(info), "W1AW", "hello") == 16);
        REQUIRE(strcmp(info, ":W1AW     :hello") == 0);

        REQUIRE((frame.len = (uint8_t)aprsFrameBuild(
                     frame.data, sizeof(frame.data), APRS_TOCALL, "N0CALL-7",
                     APRS_DEFAULT_PATH, info, strlen(info)))
                > 0);
        REQUIRE(aprsPktFromFrame(frame.data, frame.len, &pkt) == true);
        REQUIRE(aprsMsgUnwrap(&pkt, addressee, sizeof(addressee), text,
                              sizeof(text))
                == true);
        REQUIRE(strcmp(addressee, "W1AW") == 0);
        REQUIRE(strcmp(text, "hello") == 0);
    }

    SECTION("a full-width addressee needs no padding")
    {
        REQUIRE(aprsMsgFormat(info, sizeof(info), "N0CALL-15", "hi") > 0);
        REQUIRE(strcmp(info, ":N0CALL-15:hi") == 0);
    }

    SECTION("an empty message text is still a valid message")
    {
        REQUIRE(aprsMsgFormat(info, sizeof(info), "W1AW", "") == 11);
        REQUIRE(strcmp(info, ":W1AW     :") == 0);
    }

    SECTION("no acknowledgement sequence is appended")
    {
        REQUIRE(aprsMsgFormat(info, sizeof(info), "W1AW", "hello") > 0);
        REQUIRE(strchr(info, '{') == nullptr);
    }

    SECTION("invalid arguments are rejected")
    {
        char longText[APRS_MSG_TEXT_MAX + 2];
        memset(longText, 'x', sizeof(longText));
        longText[sizeof(longText) - 1] = '\0';

        REQUIRE(aprsMsgFormat(nullptr, sizeof(info), "W1AW", "hi") == 0);
        REQUIRE(aprsMsgFormat(info, 0, "W1AW", "hi") == 0);
        REQUIRE(aprsMsgFormat(info, sizeof(info), nullptr, "hi") == 0);
        REQUIRE(aprsMsgFormat(info, sizeof(info), "W1AW", nullptr) == 0);
        REQUIRE(aprsMsgFormat(info, sizeof(info), "", "hi") == 0);
        REQUIRE(aprsMsgFormat(info, sizeof(info), "TOOLONGADDR", "hi") == 0);
        REQUIRE(aprsMsgFormat(info, sizeof(info), "W1AW", longText) == 0);
        REQUIRE(aprsMsgFormat(info, 12, "W1AW", "hello") == 0);
    }
}
