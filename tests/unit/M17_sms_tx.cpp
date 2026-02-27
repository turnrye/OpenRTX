/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include "rtx/SmsTxPacket.hpp"
#include "protocols/M17/FrameEncoder.hpp"
#include "protocols/M17/FrameDecoder.hpp"
#include "protocols/M17/Datatypes.hpp"
#include "core/crc.h"

using namespace M17;

// ---------- Application-layer format tests ----------

TEST_CASE("SMS: empty message returns zero frames", "[m17][smstx]")
{
    PacketFrame frames[4];
    REQUIRE(buildSmsPacketFrames("", 0, frames, 4) == 0);
}

TEST_CASE("SMS: null message pointer returns zero frames", "[m17][smstx]")
{
    PacketFrame frames[4];
    REQUIRE(buildSmsPacketFrames(nullptr, 5, frames, 4) == 0);
}

TEST_CASE("SMS: protocol ID byte is 0x05", "[m17][smstx]")
{
    PacketFrame frames[4];
    buildSmsPacketFrames("Hi", 2, frames, 4);
    REQUIRE(frames[0][0] == 0x05);
}

TEST_CASE("SMS: message text follows protocol ID", "[m17][smstx]")
{
    PacketFrame frames[4];
    buildSmsPacketFrames("AB", 2, frames, 4);
    REQUIRE(frames[0][1] == 'A');
    REQUIRE(frames[0][2] == 'B');
}

TEST_CASE("SMS: NUL terminator follows message", "[m17][smstx]")
{
    PacketFrame frames[4];
    buildSmsPacketFrames("AB", 2, frames, 4);
    REQUIRE(frames[0][3] == 0x00);
}

TEST_CASE("SMS: CRC is computed over protocol ID + message + NUL",
          "[m17][smstx]")
{
    const char *msg = "Test CRC";
    size_t msgLen = strlen(msg);
    PacketFrame frames[4];
    size_t n = buildSmsPacketFrames(msg, msgLen, frames, 4);
    REQUIRE(n == 1);

    // Reconstruct the application data to compute expected CRC
    uint8_t appData[32];
    appData[0] = 0x05;
    memcpy(&appData[1], msg, msgLen);
    appData[1 + msgLen] = 0x00;
    size_t appDataLen = 1 + msgLen + 1;

    uint16_t expected = crc_m17(appData, appDataLen);
    uint16_t actual = (static_cast<uint16_t>(frames[0][appDataLen]) << 8)
                    | frames[0][appDataLen + 1];
    REQUIRE(actual == expected);
}

// ---------- Integration: encode/decode round-trip ----------

TEST_CASE("SMS: round-trips through encoder/decoder", "[m17][smstx]")
{
    const char *msg = "OpenRTX";
    size_t msgLen = strlen(msg);

    PacketFrame frames[4];
    size_t n = buildSmsPacketFrames(msg, msgLen, frames, 4);
    REQUIRE(n == 1);

    // Encode
    FrameEncoder encoder;
    frame_t encoded;
    encoder.encodePacketFrame(frames[0], encoded);

    // Decode
    FrameDecoder decoder;
    FrameType type = decoder.decodeFrame(encoded);
    REQUIRE(type == FrameType::PACKET);

    const PacketFrame &decoded = decoder.getPacketFrame();
    REQUIRE(decoded[0] == 0x05);

    for (size_t i = 0; i < msgLen; i++)
        REQUIRE(decoded[1 + i] == static_cast<uint8_t>(msg[i]));

    REQUIRE(decoded[1 + msgLen] == 0x00);
}

// ---------- Reference vector ----------

TEST_CASE("SMS: reference vector matches known-good frame", "[m17][smstx]")
{
    // The known-good encoded frame for "OpenRTX" from M17_packet.cpp
    static constexpr frame_t KNOWN_GOOD_FRAME = {
        0x75, 0xFF, 0xF6, 0xD4, 0xC2, 0x59, 0x82, 0x96, 0x84, 0x63, 0x9A, 0x26,
        0xF6, 0xD8, 0xB8, 0xF0, 0x9D, 0x0D, 0x4C, 0xD0, 0x00, 0x03, 0x9F, 0x05,
        0xEE, 0x6E, 0x72, 0x2F, 0x23, 0xDA, 0x96, 0xFE, 0xCB, 0x70, 0x9B, 0x09,
        0x52, 0x03, 0xD7, 0xB3, 0xA2, 0xB2, 0x52, 0xB9, 0xAD, 0xA9, 0x39, 0xA3
    };

    PacketFrame frames[4];
    size_t n = buildSmsPacketFrames("OpenRTX", 7, frames, 4);
    REQUIRE(n == 1);

    FrameEncoder encoder;
    frame_t encoded;
    encoder.encodePacketFrame(frames[0], encoded);
    REQUIRE(encoded == KNOWN_GOOD_FRAME);
}
