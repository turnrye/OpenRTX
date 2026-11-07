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

#include <cerrno>
#include <cstring>

#include "aprs_test_frame.h"
#include "protocols/APRS/packet.h"

TEST_CASE("APRS packet: destination and source are unshifted", "[aprs]")
{
    struct aprsTestFrame frame = aprsTest_makeFrame("APRS", 0, "N2BP", 7,
                                                    ">hello");
    aprsPacket pkt;

    REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == 0);
    REQUIRE(pkt.addressesLen == 2);
    REQUIRE(strcmp(pkt.addresses[0].addr, "APRS") == 0);
    REQUIRE(pkt.addresses[0].ssid == 0);
    REQUIRE(strcmp(pkt.addresses[1].addr, "N2BP") == 0);
    REQUIRE(pkt.addresses[1].ssid == 7);
    REQUIRE(memcmp(pkt.info, ">hello", pkt.infoLen) == 0);
    REQUIRE(pkt.infoLen == 6);
}

TEST_CASE("APRS packet: address formatting follows the TNC2 convention",
          "[aprs]")
{
    struct aprsTestFrame frame = aprsTest_makeFrame("APRS", 0, "N2BP", 7,
                                                    ">hi");
    aprsPacket pkt;
    char buf[APRS_ADDR_STR_LEN];

    REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == 0);

    /* SSID 0 is left off entirely. */
    REQUIRE(aprsAddr_toStr(&pkt.addresses[0], buf, sizeof(buf)) == 4);
    REQUIRE(strcmp(buf, "APRS") == 0);

    REQUIRE(aprsAddr_toStr(&pkt.addresses[1], buf, sizeof(buf)) == 6);
    REQUIRE(strcmp(buf, "N2BP-7") == 0);
}

TEST_CASE("APRS packet: the longest callsign-SSID pair still fits", "[aprs]")
{
    struct aprsTestFrame frame = aprsTest_makeFrame("APRS", 0, "ABCDEF", 15,
                                                    ">hi");
    aprsPacket pkt;
    char buf[APRS_ADDR_STR_LEN];

    REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == 0);
    REQUIRE(aprsAddr_toStr(&pkt.addresses[1], buf, sizeof(buf)) == 9);
    REQUIRE(strcmp(buf, "ABCDEF-15") == 0);
}

TEST_CASE("APRS packet: digipeater path is formatted with repeat markers",
          "[aprs]")
{
    struct aprsTestFrame frame;
    memset(&frame, 0, sizeof(frame));

    aprsTest_putAddress(frame.data + 0, "APRS", 0, false, false);
    aprsTest_putAddress(frame.data + 7, "N2BP", 7, false, false);
    aprsTest_putAddress(frame.data + 14, "WIDE1", 1, true, false);
    aprsTest_putAddress(frame.data + 21, "WIDE2", 2, false, true);
    frame.data[28] = 0x03;
    frame.data[29] = 0xf0;
    memcpy(frame.data + 30, ">hi", 3);
    frame.len = 33;

    aprsPacket pkt;
    REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == 0);
    REQUIRE(pkt.addressesLen == 4);

    char path[32];
    const size_t len = aprsPath_toStr(&pkt, path, sizeof(path));
    REQUIRE(len == strlen(path));
    REQUIRE(strcmp(path, "WIDE1-1*,WIDE2-2") == 0);
}

TEST_CASE("APRS packet: a directly heard packet has an empty path", "[aprs]")
{
    struct aprsTestFrame frame = aprsTest_makeFrame("APRS", 0, "N2BP", 7,
                                                    ">hi");
    aprsPacket pkt;
    char path[32];

    REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == 0);
    REQUIRE(aprsPath_toStr(&pkt, path, sizeof(path)) == 0);
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
        struct aprsTestFrame frame = aprsTest_makeFrame("APRS", 0, "N2BP", 7,
                                                        c.info);
        aprsPacket pkt;

        INFO("info field: " << c.info);
        REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == 0);
        REQUIRE(pkt.type == c.type);
    }
}

TEST_CASE("APRS packet: an addressed message is split into addressee and text",
          "[aprs]")
{
    struct aprsTestFrame frame = aprsTest_makeFrame("APRS", 0, "N2BP", 7,
                                                    ":W1AW     :hello there");
    aprsPacket pkt;
    char addressee[APRS_ADDR_STR_LEN];
    char text[APRS_PACLEN];

    REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == 0);
    REQUIRE(pkt.type == APRS_TYPE_MESSAGE);
    REQUIRE(
        aprsMsg_unwrap(&pkt, addressee, sizeof(addressee), text, sizeof(text))
        == 0);
    REQUIRE(strcmp(addressee, "W1AW") == 0);
    REQUIRE(strcmp(text, "hello there") == 0);
}

TEST_CASE("APRS packet: the acknowledgement sequence is stripped", "[aprs]")
{
    struct aprsTestFrame frame = aprsTest_makeFrame("APRS", 0, "N2BP", 7,
                                                    ":W1AW     :ping{042");
    aprsPacket pkt;
    char addressee[APRS_ADDR_STR_LEN];
    char text[APRS_PACLEN];

    REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == 0);
    REQUIRE(
        aprsMsg_unwrap(&pkt, addressee, sizeof(addressee), text, sizeof(text))
        == 0);
    REQUIRE(strcmp(addressee, "W1AW") == 0);
    REQUIRE(strcmp(text, "ping") == 0);
}

TEST_CASE("APRS packet: a full-width addressee keeps every character", "[aprs]")
{
    struct aprsTestFrame frame = aprsTest_makeFrame("APRS", 0, "N2BP", 7,
                                                    ":ABCDEF-15:x");
    aprsPacket pkt;
    char addressee[APRS_ADDR_STR_LEN];
    char text[APRS_PACLEN];

    REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == 0);
    REQUIRE(
        aprsMsg_unwrap(&pkt, addressee, sizeof(addressee), text, sizeof(text))
        == 0);
    REQUIRE(strcmp(addressee, "ABCDEF-15") == 0);
    REQUIRE(strcmp(text, "x") == 0);
}

TEST_CASE("APRS packet: an empty message body unwraps to an empty string",
          "[aprs]")
{
    struct aprsTestFrame frame = aprsTest_makeFrame("APRS", 0, "N2BP", 7,
                                                    ":W1AW     :");
    aprsPacket pkt;
    char addressee[APRS_ADDR_STR_LEN];
    char text[APRS_PACLEN];

    REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == 0);
    REQUIRE(
        aprsMsg_unwrap(&pkt, addressee, sizeof(addressee), text, sizeof(text))
        == 0);
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
        struct aprsTestFrame frame = aprsTest_makeFrame("APRS", 0, "N2BP", 7,
                                                        ":not a message");
        REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == 0);
        REQUIRE(pkt.type == APRS_TYPE_MESSAGE);
        REQUIRE(aprsMsg_unwrap(&pkt, addressee, sizeof(addressee), text,
                               sizeof(text))
                == -EINVAL);
    }

    SECTION("too short to hold an addressee")
    {
        struct aprsTestFrame frame = aprsTest_makeFrame("APRS", 0, "N2BP", 7,
                                                        ":W1AW");
        REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == 0);
        REQUIRE(aprsMsg_unwrap(&pkt, addressee, sizeof(addressee), text,
                               sizeof(text))
                == -EINVAL);
    }

    SECTION("blank addressee")
    {
        struct aprsTestFrame frame = aprsTest_makeFrame("APRS", 0, "N2BP", 7,
                                                        ":         :hi");
        REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == 0);
        REQUIRE(aprsMsg_unwrap(&pkt, addressee, sizeof(addressee), text,
                               sizeof(text))
                == -EINVAL);
    }

    SECTION("not a message at all")
    {
        struct aprsTestFrame frame = aprsTest_makeFrame("APRS", 0, "N2BP", 7,
                                                        ">status text");
        REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == 0);
        REQUIRE(aprsMsg_unwrap(&pkt, addressee, sizeof(addressee), text,
                               sizeof(text))
                == -EINVAL);
    }
}

TEST_CASE("APRS packet: malformed frames are rejected", "[aprs]")
{
    aprsPacket pkt;

    SECTION("too short for two addresses")
    {
        struct aprsTestFrame frame;
        memset(&frame, 0, sizeof(frame));
        aprsTest_putAddress(frame.data, "APRS", 0, false, true);
        frame.len = 7;
        REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == -EINVAL);
    }

    SECTION("address field is never terminated")
    {
        struct aprsTestFrame frame;
        memset(&frame, 0, sizeof(frame));
        for (uint8_t i = 0; i < 4; i++)
            aprsTest_putAddress(frame.data + i * 7, "NOEND", 0, false, false);
        frame.len = 30;
        REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == -EINVAL);
    }

    SECTION("more addresses than AX.25 allows")
    {
        struct aprsTestFrame frame;
        memset(&frame, 0, sizeof(frame));
        for (uint8_t i = 0; i < APRS_MAX_ADDRESSES + 1; i++)
            aprsTest_putAddress(frame.data + i * 7, "RELAY", 0, false, false);
        frame.len = 7 * (APRS_MAX_ADDRESSES + 1);
        REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == -EINVAL);
    }

    SECTION("no room left for an info field")
    {
        struct aprsTestFrame frame = aprsTest_makeFrame("APRS", 0, "N2BP", 7,
                                                        "");
        REQUIRE(frame.len == 16);
        REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == -EINVAL);
    }
}

TEST_CASE("APRS packet: the info field may be as long as AX.25 allows",
          "[aprs]")
{
    char info[APRS_MAX_INFO + 2];
    struct aprsPacket pkt;

    memset(info, 'x', sizeof(info) - 1);
    info[0] = '>';

    SECTION("256 bytes are kept whole")
    {
        info[APRS_MAX_INFO] = '\0';
        struct aprsTestFrame frame = aprsTest_makeFrame("APORT1", 0, "K1ABC", 9,
                                                        info);

        REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == 0);
        CHECK(pkt.infoLen == APRS_MAX_INFO);
        CHECK(memcmp(pkt.info, info, APRS_MAX_INFO) == 0);
    }

    SECTION("257 bytes are rejected")
    {
        info[APRS_MAX_INFO + 1] = '\0';
        struct aprsTestFrame frame = aprsTest_makeFrame("APORT1", 0, "K1ABC", 9,
                                                        info);

        REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == -EINVAL);
    }
}

TEST_CASE("APRS address: TNC2 text parses back into an address", "[aprs]")
{
    struct aprsAddress addr;

    SECTION("callsign without an SSID")
    {
        REQUIRE(aprsAddr_fromStr("W1AW", &addr) == 0);
        REQUIRE(strcmp(addr.addr, "W1AW") == 0);
        REQUIRE(addr.ssid == 0);
    }

    SECTION("callsign with an SSID")
    {
        REQUIRE(aprsAddr_fromStr("N0CALL-15", &addr) == 0);
        REQUIRE(strcmp(addr.addr, "N0CALL") == 0);
        REQUIRE(addr.ssid == 15);
    }

    SECTION("lower case and a leading zero, as other stations may write")
    {
        REQUIRE(aprsAddr_fromStr("n0call-07", &addr) == 0);
        REQUIRE(strcmp(addr.addr, "N0CALL") == 0);
        REQUIRE(addr.ssid == 7);
    }

    SECTION("round trip through the formatter")
    {
        char out[APRS_ADDR_STR_LEN];
        REQUIRE(aprsAddr_fromStr("N0CALL-7", &addr) == 0);
        REQUIRE(aprsAddr_toStr(&addr, out, sizeof(out)) == 8);
        REQUIRE(strcmp(out, "N0CALL-7") == 0);
    }

    SECTION("malformed text is rejected")
    {
        REQUIRE(aprsAddr_fromStr("", &addr) == -EINVAL);
        REQUIRE(aprsAddr_fromStr("-7", &addr) == -EINVAL);
        REQUIRE(aprsAddr_fromStr("N0CALL-", &addr) == -EINVAL);
        REQUIRE(aprsAddr_fromStr("N0CALL-16", &addr) == -EINVAL);
        REQUIRE(aprsAddr_fromStr("N0CALL-x", &addr) == -EINVAL);
        REQUIRE(aprsAddr_fromStr("TOOLONGCALL", &addr) == -EINVAL);
        REQUIRE(aprsAddr_fromStr("WITH SPACE", &addr) == -EINVAL);
    }
}

TEST_CASE("APRS address: a callsign the user chooses is checked strictly",
          "[aprs]")
{
    SECTION("station addresses as APRS writes them")
    {
        CHECK(aprsAddr_validate("W1AW") == 0);
        CHECK(aprsAddr_validate("N0CALL-7") == 0);
        CHECK(aprsAddr_validate("ABCDEF-15") == 0);
        CHECK(aprsAddr_validate("K1ABC-0") == 0);
    }

    SECTION("anything an AX.25 address cannot carry")
    {
        CHECK(aprsAddr_validate("") == -EINVAL);
        CHECK(aprsAddr_validate("w1aw") == -EINVAL);
        CHECK(aprsAddr_validate("W1AW/P") == -EINVAL);
        CHECK(aprsAddr_validate("ABCDEFG") == -EINVAL);
        CHECK(aprsAddr_validate("N0CALL-") == -EINVAL);
        CHECK(aprsAddr_validate("N0CALL-07") == -EINVAL);
        CHECK(aprsAddr_validate("N0CALL-16") == -EINVAL);
        CHECK(aprsAddr_validate("N0CALL-7X") == -EINVAL);
        CHECK(aprsAddr_validate("-7") == -EINVAL);
    }
}

TEST_CASE("APRS frame builder: a built frame parses back to what went in",
          "[aprs]")
{
    struct aprsTestFrame frame;
    aprsPacket pkt;
    char addr[APRS_ADDR_STR_LEN];
    char path[32];

    const char info[] = ":W1AW     :hello";

    REQUIRE((frame.len = aprsFrame_build(frame.data, sizeof(frame.data),
                                         APRS_TOCALL, "N0CALL-7",
                                         APRS_DEFAULT_PATH, info, strlen(info)))
            > 0);
    REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == 0);

    REQUIRE(pkt.addressesLen == 4);

    aprsAddr_toStr(&pkt.addresses[0], addr, sizeof(addr));
    REQUIRE(strcmp(addr, APRS_TOCALL) == 0);

    aprsAddr_toStr(&pkt.addresses[1], addr, sizeof(addr));
    REQUIRE(strcmp(addr, "N0CALL-7") == 0);

    aprsPath_toStr(&pkt, path, sizeof(path));
    REQUIRE(strcmp(path, APRS_DEFAULT_PATH) == 0);

    REQUIRE(pkt.type == APRS_TYPE_MESSAGE);
    REQUIRE(std::string(pkt.info, pkt.infoLen) == info);
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

    struct aprsTestFrame frame;
    const char info[] = ":W1AW     :hello";

    REQUIRE((frame.len = aprsFrame_build(frame.data, sizeof(frame.data),
                                         APRS_TOCALL, "N0CALL-7",
                                         APRS_DEFAULT_PATH, info, strlen(info)))
            > 0);

    REQUIRE(frame.len == sizeof(expected));
    REQUIRE(memcmp(frame.data, expected, sizeof(expected)) == 0);
}

TEST_CASE("APRS frame builder: a path is optional", "[aprs]")
{
    struct aprsTestFrame frame;
    aprsPacket pkt;
    char path[32];

    const char info[] = ">heard direct";

    REQUIRE((frame.len = aprsFrame_build(frame.data, sizeof(frame.data),
                                         APRS_TOCALL, "N0CALL", nullptr, info,
                                         strlen(info)))
            > 0);
    REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == 0);
    REQUIRE(pkt.addressesLen == 2);
    REQUIRE(aprsPath_toStr(&pkt, path, sizeof(path)) == 0);

    REQUIRE((frame.len = aprsFrame_build(frame.data, sizeof(frame.data),
                                         APRS_TOCALL, "N0CALL", "", info,
                                         strlen(info)))
            > 0);
    REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == 0);
    REQUIRE(pkt.addressesLen == 2);
}

TEST_CASE("APRS frame builder: a repeated path element round-trips", "[aprs]")
{
    struct aprsTestFrame frame;
    aprsPacket pkt;
    char path[32];

    const char info[] = ">digipeated";

    REQUIRE((frame.len = aprsFrame_build(
                 frame.data, sizeof(frame.data), APRS_TOCALL, "N0CALL",
                 "WIDE1-1*,WIDE2-1", info, strlen(info)))
            > 0);
    REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == 0);
    REQUIRE(aprsPath_toStr(&pkt, path, sizeof(path)) > 0);
    REQUIRE(strcmp(path, "WIDE1-1*,WIDE2-1") == 0);
}

TEST_CASE("APRS frame builder: invalid arguments are rejected", "[aprs]")
{
    struct aprsTestFrame frame;
    const char info[] = ">hi";

    SECTION("an empty info field would not parse back")
    {
        REQUIRE(aprsFrame_build(frame.data, sizeof(frame.data), APRS_TOCALL,
                                "N0CALL", nullptr, info, 0)
                == -EINVAL);
    }

    SECTION("a malformed source or path")
    {
        REQUIRE(aprsFrame_build(frame.data, sizeof(frame.data), APRS_TOCALL,
                                "N0CALL-99", nullptr, info, strlen(info))
                == -EINVAL);
        REQUIRE(aprsFrame_build(frame.data, sizeof(frame.data), APRS_TOCALL,
                                "N0CALL", "WIDE1-1,", info, strlen(info))
                == -EINVAL);
        REQUIRE(aprsFrame_build(frame.data, sizeof(frame.data), APRS_TOCALL,
                                "N0CALL", "TOOLONGDIGI", info, strlen(info))
                == -EINVAL);
    }

    SECTION("more digipeaters than AX.25 allows")
    {
        REQUIRE(aprsFrame_build(frame.data, sizeof(frame.data), APRS_TOCALL,
                                "N0CALL", "W1,W2,W3,W4,W5,W6,W7,W8,W9", info,
                                strlen(info))
                == -EINVAL);
    }

    SECTION("an info field longer than AX.25 allows")
    {
        char big[APRS_MAX_INFO + 1];
        memset(big, 'x', sizeof(big));
        REQUIRE(aprsFrame_build(frame.data, sizeof(frame.data), APRS_TOCALL,
                                "N0CALL", APRS_DEFAULT_PATH, big, sizeof(big))
                == -EINVAL);
    }

    SECTION("a frame that would not fit the buffer")
    {
        REQUIRE(aprsFrame_build(frame.data, 16, APRS_TOCALL, "N0CALL", nullptr,
                                info, strlen(info))
                == -ENOSPC);
    }
}

TEST_CASE("APRS message: a formatted message unwraps to what went in", "[aprs]")
{
    char info[APRS_MSG_ADDRESSEE_LEN + APRS_MSG_TEXT_MAX + 3];
    struct aprsTestFrame frame;
    aprsPacket pkt;
    char addressee[APRS_ADDR_STR_LEN];
    char text[APRS_PACLEN];

    SECTION("a short addressee is padded to nine characters")
    {
        REQUIRE(aprsMsg_format(info, sizeof(info), "W1AW", "hello") == 16);
        REQUIRE(strcmp(info, ":W1AW     :hello") == 0);

        REQUIRE((frame.len = aprsFrame_build(
                     frame.data, sizeof(frame.data), APRS_TOCALL, "N0CALL-7",
                     APRS_DEFAULT_PATH, info, strlen(info)))
                > 0);
        REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == 0);
        REQUIRE(aprsMsg_unwrap(&pkt, addressee, sizeof(addressee), text,
                               sizeof(text))
                == 0);
        REQUIRE(strcmp(addressee, "W1AW") == 0);
        REQUIRE(strcmp(text, "hello") == 0);
    }

    SECTION("a full-width addressee needs no padding")
    {
        REQUIRE(aprsMsg_format(info, sizeof(info), "N0CALL-15", "hi") > 0);
        REQUIRE(strcmp(info, ":N0CALL-15:hi") == 0);
    }

    SECTION("an empty message text is still a valid message")
    {
        REQUIRE(aprsMsg_format(info, sizeof(info), "W1AW", "") == 11);
        REQUIRE(strcmp(info, ":W1AW     :") == 0);
    }

    SECTION("no acknowledgement sequence is appended")
    {
        REQUIRE(aprsMsg_format(info, sizeof(info), "W1AW", "hello") > 0);
        REQUIRE(strchr(info, '{') == nullptr);
    }

    SECTION("invalid arguments are rejected")
    {
        char longText[APRS_MSG_TEXT_MAX + 2];
        memset(longText, 'x', sizeof(longText));
        longText[sizeof(longText) - 1] = '\0';

        REQUIRE(aprsMsg_format(info, 0, "W1AW", "hi") == -ENOSPC);
        REQUIRE(aprsMsg_format(info, sizeof(info), "", "hi") == -EINVAL);
        REQUIRE(aprsMsg_format(info, sizeof(info), "TOOLONGADDR", "hi")
                == -EINVAL);
        REQUIRE(aprsMsg_format(info, sizeof(info), "W1AW", longText)
                == -EINVAL);
        REQUIRE(aprsMsg_format(info, 12, "W1AW", "hello") == -ENOSPC);
    }
}

TEST_CASE("APRS TNC2: a frame is shown as a monitor line", "[aprs]")
{
    auto line = [](const char *path, const char *info) {
        uint8_t frame[APRS_PACLEN];
        ssize_t n = aprsFrame_build(frame, APRS_PACLEN, "APORT1", "K1ABC-9",
                                    path, info, strlen(info));
        REQUIRE(n > 0);
        char out[APRS_PACLEN + 64];
        size_t m = aprsFrameToTnc2(frame, n, out, sizeof(out));
        REQUIRE(m == strlen(out));
        return std::string(out);
    };

    CHECK(line(nullptr, ">hi") == "K1ABC-9>APORT1:>hi");
    CHECK(line("WIDE1-1,WIDE2-1", ":W1AW     :yo{01")
          == "K1ABC-9>APORT1,WIDE1-1,WIDE2-1::W1AW     :yo{01");

    /* Only the last used hop is starred, as TNC2 writes it */
    CHECK(line("N2GH*,W2UB*,WIDE2-1", "x")
          == "K1ABC-9>APORT1,N2GH,W2UB*,WIDE2-1:x");

    /* Back again to the same frame */
    uint8_t frame[APRS_PACLEN], again[APRS_PACLEN];
    const char *info = "=4903.50N/07201.75W-";
    ssize_t n = aprsFrame_build(frame, APRS_PACLEN, "APORT1", "K1ABC-9",
                                "WIDE1-1", info, strlen(info));
    char out[APRS_PACLEN + 64];
    size_t m = aprsFrameToTnc2(frame, n, out, sizeof(out));
    CHECK(aprsFrameFromTnc2(out, m, again, sizeof(again)) == n);
    CHECK(memcmp(frame, again, n) == 0);

    /* Too small, or not a frame */
    CHECK(aprsFrameToTnc2(frame, n, out, 10) == 0);
    CHECK(aprsFrameToTnc2(frame, 5, out, sizeof(out)) == 0);
}

TEST_CASE("APRS TNC2: a monitor line builds the frame it shows", "[aprs]")
{
    auto build = [](const char *line) {
        uint8_t frame[APRS_PACLEN];
        size_t n = aprsFrameFromTnc2(line, strlen(line), frame, sizeof(frame));
        if (n == 0)
            return std::string();
        struct aprsPacket pkt;
        REQUIRE(aprsPkt_fromFrame(frame, n, &pkt) == 0);
        char src[APRS_ADDR_STR_LEN], dst[APRS_ADDR_STR_LEN], path[80];
        aprsAddr_toStr(&pkt.addresses[1], src, sizeof(src));
        aprsAddr_toStr(&pkt.addresses[0], dst, sizeof(dst));
        aprsPath_toStr(&pkt, path, sizeof(path));
        std::string out = std::string(src) + ">" + dst;
        if (path[0] != '\0')
            out += std::string(",") + path;
        return out + ":" + std::string(pkt.info, pkt.infoLen);
    };

    CHECK(build("W1AW>APRS:>status") == "W1AW>APRS:>status");
    CHECK(build("N2BP-7>APORT1,WIDE1-1,WIDE2-1::K1ABC    :hi{01")
          == "N2BP-7>APORT1,WIDE1-1,WIDE2-1::K1ABC    :hi{01");
    CHECK(build("W9XYZ>APZ,N2GH*,WIDE2-1:x") == "W9XYZ>APZ,N2GH*,WIDE2-1:x");

    /* Colons in the info stay in the info */
    CHECK(build("W1AW>APRS::W2UB     :a:b") == "W1AW>APRS::W2UB     :a:b");

    CHECK(build("") == "");
    CHECK(build("W1AW:no destination") == "");
    CHECK(build(">APRS:no source") == "");
    CHECK(build("W1AW>APRS:") == "");
    CHECK(build("W1AW>APRS,BAD ADDR:x") == "");
}

TEST_CASE("APRS path: a typed digipeater path is a list of valid addresses",
          "[aprs]")
{
    const char *valid[] = {
        "", "WIDE1-1", "WIDE1-1,WIDE2-1", "W1AW-1,WIDE2-2", "A,B,C,D,E,F,G,H",
    };
    const char *invalid[] = {
        ",",       "WIDE1-1,",        ",WIDE1-1", "WIDE1-1,,WIDE2-1",
        "wide1-1", "WIDE1-1 WIDE2-1", "WIDE1-1*", "A,B,C,D,E,F,G,H,I",
        "ABCDEFG",
    };

    for (const char *path : valid) {
        INFO("path: " << path);
        CHECK(aprsPathValid(path) == true);
    }
    for (const char *path : invalid) {
        INFO("path: " << path);
        CHECK(aprsPathValid(path) == false);
    }
    CHECK(aprsPathValid(nullptr) == false);
}
