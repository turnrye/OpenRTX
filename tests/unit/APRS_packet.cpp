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
