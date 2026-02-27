/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include "protocols/M17/PacketAssembler.hpp"
#include "protocols/M17/FrameEncoder.hpp"
#include "protocols/M17/FrameDecoder.hpp"
#include "protocols/M17/Datatypes.hpp"
#include "core/crc.h"

using namespace M17;

// ---------- Input validation tests ----------

TEST_CASE("buildPacketFrames: null data returns zero", "[m17][framing]")
{
    PacketFrame frames[4];
    REQUIRE(buildPacketFrames(nullptr, 10, frames, 4) == 0);
}

TEST_CASE("buildPacketFrames: zero length returns zero", "[m17][framing]")
{
    uint8_t data[] = { 0x05 };
    PacketFrame frames[4];
    REQUIRE(buildPacketFrames(data, 0, frames, 4) == 0);
}

TEST_CASE("buildPacketFrames: null frames returns zero", "[m17][framing]")
{
    uint8_t data[] = { 0x05, 'H', 'i', 0x00 };
    REQUIRE(buildPacketFrames(data, 4, nullptr, 4) == 0);
}

TEST_CASE("buildPacketFrames: zero maxFrames returns zero", "[m17][framing]")
{
    uint8_t data[] = { 0x05, 'H', 'i', 0x00 };
    PacketFrame frames[4];
    REQUIRE(buildPacketFrames(data, 4, frames, 0) == 0);
}

// ---------- CRC tests ----------

TEST_CASE("buildPacketFrames: CRC matches crc_m17 over input data",
          "[m17][framing]")
{
    uint8_t data[] = { 0x05, 'T', 'e', 's', 't', 0x00 };
    size_t dataLen = sizeof(data);

    PacketFrame frames[4];
    size_t n = buildPacketFrames(data, dataLen, frames, 4);
    REQUIRE(n == 1);

    uint16_t expected = crc_m17(data, dataLen);
    uint16_t actual = (static_cast<uint16_t>(frames[0][dataLen]) << 8)
                    | frames[0][dataLen + 1];
    REQUIRE(actual == expected);
}

TEST_CASE("buildPacketFrames: CRC is big-endian", "[m17][framing]")
{
    uint8_t data[] = { 0x01, 0x02, 0x03 };
    PacketFrame frames[4];
    buildPacketFrames(data, 3, frames, 4);

    uint16_t crc = crc_m17(data, 3);
    REQUIRE(frames[0][3] == static_cast<uint8_t>(crc >> 8));
    REQUIRE(frames[0][4] == static_cast<uint8_t>(crc & 0xFF));
}

// ---------- Single-frame tests ----------

TEST_CASE("buildPacketFrames: short data fits in one frame", "[m17][framing]")
{
    uint8_t data[] = { 0x05, 'H', 'i', 0x00 };
    // total = 4 + 2(CRC) = 6 bytes -> 1 frame
    PacketFrame frames[4];
    size_t n = buildPacketFrames(data, 4, frames, 4);
    REQUIRE(n == 1);
    REQUIRE(frames[0].isEof() == true);
    REQUIRE(frames[0].getCounter() == 6);
}

TEST_CASE("buildPacketFrames: data bytes are copied correctly",
          "[m17][framing]")
{
    uint8_t data[] = { 0xAA, 0xBB, 0xCC };
    PacketFrame frames[4];
    buildPacketFrames(data, 3, frames, 4);
    REQUIRE(frames[0][0] == 0xAA);
    REQUIRE(frames[0][1] == 0xBB);
    REQUIRE(frames[0][2] == 0xCC);
}

TEST_CASE("buildPacketFrames: padding after CRC is zero", "[m17][framing]")
{
    uint8_t data[] = { 0x05, 'A', 0x00 };
    // total = 3 + 2 = 5 bytes, 20 padding bytes
    PacketFrame frames[4];
    buildPacketFrames(data, 3, frames, 4);
    for (size_t i = 5; i < PacketFrame::DATA_SIZE; i++) {
        REQUIRE(frames[0][i] == 0);
    }
}

TEST_CASE("buildPacketFrames: exactly 25-byte total produces one frame",
          "[m17][framing]")
{
    // 23 data bytes + 2 CRC = 25
    uint8_t data[23];
    memset(data, 0x42, sizeof(data));

    PacketFrame frames[4];
    size_t n = buildPacketFrames(data, 23, frames, 4);
    REQUIRE(n == 1);
    REQUIRE(frames[0].isEof() == true);
    REQUIRE(frames[0].getCounter() == 25);
}

// ---------- Multi-frame tests ----------

TEST_CASE("buildPacketFrames: data spanning two frames", "[m17][framing]")
{
    // 24 data bytes + 2 CRC = 26 bytes -> 2 frames (25 + 1)
    uint8_t data[24];
    memset(data, 0x55, sizeof(data));

    PacketFrame frames[4];
    size_t n = buildPacketFrames(data, 24, frames, 4);
    REQUIRE(n == 2);

    REQUIRE(frames[0].isEof() == false);
    REQUIRE(frames[0].getCounter() == 0);

    REQUIRE(frames[1].isEof() == true);
    REQUIRE(frames[1].getCounter() == 1);
}

TEST_CASE("buildPacketFrames: three frames with sequential counters",
          "[m17][framing]")
{
    // 50 data bytes + 2 CRC = 52 -> 3 frames (25, 25, 2)
    uint8_t data[50];
    memset(data, 0x77, sizeof(data));

    PacketFrame frames[4];
    size_t n = buildPacketFrames(data, 50, frames, 4);
    REQUIRE(n == 3);

    REQUIRE(frames[0].getCounter() == 0);
    REQUIRE(frames[0].isEof() == false);

    REQUIRE(frames[1].getCounter() == 1);
    REQUIRE(frames[1].isEof() == false);

    REQUIRE(frames[2].isEof() == true);
    REQUIRE(frames[2].getCounter() == 2);
}

TEST_CASE("buildPacketFrames: exactly 50-byte total produces two frames",
          "[m17][framing]")
{
    // 48 data bytes + 2 CRC = 50 -> 2 frames (25 + 25)
    uint8_t data[48];
    memset(data, 0xDD, sizeof(data));

    PacketFrame frames[4];
    size_t n = buildPacketFrames(data, 48, frames, 4);
    REQUIRE(n == 2);

    REQUIRE(frames[0].isEof() == false);
    REQUIRE(frames[0].getCounter() == 0);

    REQUIRE(frames[1].isEof() == true);
    REQUIRE(frames[1].getCounter() == 25);
}

TEST_CASE("buildPacketFrames: maxFrames too small returns zero",
          "[m17][framing]")
{
    // Needs 2 frames but only 1 allowed
    uint8_t data[24];
    memset(data, 0xAA, sizeof(data));

    PacketFrame frames[1];
    REQUIRE(buildPacketFrames(data, 24, frames, 1) == 0);
}

// ---------- Encoder/decoder round-trip ----------

TEST_CASE("buildPacketFrames: single frame round-trips through encoder/decoder",
          "[m17][framing]")
{
    uint8_t data[] = { 0x05, 'O', 'p', 'e', 'n', 'R', 'T', 'X', 0x00 };
    PacketFrame frames[4];
    size_t n = buildPacketFrames(data, sizeof(data), frames, 4);
    REQUIRE(n == 1);

    FrameEncoder encoder;
    frame_t encoded;
    encoder.encodePacketFrame(frames[0], encoded);

    FrameDecoder decoder;
    FrameType type = decoder.decodeFrame(encoded);
    REQUIRE(type == FrameType::PACKET);

    const PacketFrame &decoded = decoder.getPacketFrame();
    for (size_t i = 0; i < sizeof(data); i++)
        REQUIRE(decoded[i] == data[i]);

    REQUIRE(decoded.isEof() == true);
    REQUIRE(decoded.getCounter() == sizeof(data) + 2);
}

TEST_CASE("buildPacketFrames: multi-frame round-trips through encoder/decoder",
          "[m17][framing]")
{
    // 30 bytes data + 2 CRC = 32 -> 2 frames (25 + 7)
    uint8_t data[30];
    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = i;

    PacketFrame frames[4];
    size_t n = buildPacketFrames(data, sizeof(data), frames, 4);
    REQUIRE(n == 2);

    FrameEncoder encoder;
    FrameDecoder decoder;

    for (size_t i = 0; i < n; i++) {
        frame_t encoded;
        encoder.encodePacketFrame(frames[i], encoded);

        FrameType type = decoder.decodeFrame(encoded);
        REQUIRE(type == FrameType::PACKET);

        const PacketFrame &decoded = decoder.getPacketFrame();
        REQUIRE(memcmp(frames[i].data(), decoded.data(), PacketFrame::DATA_SIZE)
                == 0);
        REQUIRE(decoded.isEof() == frames[i].isEof());
        REQUIRE(decoded.getCounter() == frames[i].getCounter());
    }
}

TEST_CASE("buildPacketFrames: works with binary data including embedded nulls",
          "[m17][framing]")
{
    // Protocol 0x00 (RAW) with binary content including null bytes
    uint8_t data[] = { 0x00, 0xFF, 0x00, 0xAB, 0x00, 0xCD };
    PacketFrame frames[4];
    size_t n = buildPacketFrames(data, sizeof(data), frames, 4);
    REQUIRE(n == 1);
    for (size_t i = 0; i < sizeof(data); i++)
        REQUIRE(frames[0][i] == data[i]);
}
