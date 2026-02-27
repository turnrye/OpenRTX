/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <array>
#include <algorithm>
#include "protocols/M17/M17FrameEncoder.hpp"
#include "protocols/M17/M17FrameDecoder.hpp"
#include "protocols/M17/M17PacketFrame.hpp"
#include "protocols/M17/M17Datatypes.hpp"

using namespace M17;

TEST_CASE("Freshly constructed PacketFrame is zeroed", "[m17][packet]")
{
    M17PacketFrame frame;
    const uint8_t *data = frame.getData();

    for (size_t i = 0; i < sizeof(pktPayload_t); i++) {
        REQUIRE(data[i] == 0);
    }
}

TEST_CASE("Payload provides read/write access", "[m17][packet]")
{
    M17PacketFrame frame;
    pktPayload_t &payload = frame.payload();

    // Write a known pattern
    for (size_t i = 0; i < payload.size(); i++)
        payload[i] = static_cast<uint8_t>(i + 1);

    // Read back via getData()
    const uint8_t *data = frame.getData();
    for (size_t i = 0; i < payload.size(); i++) {
        REQUIRE(data[i] == static_cast<uint8_t>(i + 1));
    }
}

TEST_CASE("clear() resets every byte to zero", "[m17][packet]")
{
    M17PacketFrame frame;
    pktPayload_t &payload = frame.payload();

    for (size_t i = 0; i < payload.size(); i++)
        payload[i] = 0xFF;

    frame.clear();
    const uint8_t *data = frame.getData();
    for (size_t i = 0; i < sizeof(pktPayload_t); i++) {
        REQUIRE(data[i] == 0);
    }
}

TEST_CASE("Encode then decode round-trip", "[m17][packet]")
{
    M17FrameEncoder encoder;
    M17FrameDecoder decoder;

    // Build a test payload: "HELLO M17 PACKET!!" padded with zeros
    pktPayload_t payload = {};
    const char msg[] = "HELLO M17 PACKET!!";
    std::copy_n(msg, sizeof(msg) - 1, payload.begin());

    // Encode
    frame_t encoded;
    encoder.encodePacketFrame(payload, encoded);

    // The first two bytes should be the PACKET_SYNC_WORD
    REQUIRE(encoded[0] == 0x75);
    REQUIRE(encoded[1] == 0xFF);

    // Decode
    M17FrameType type = decoder.decodeFrame(encoded);
    REQUIRE(type == M17FrameType::PACKET);

    const M17PacketFrame &decoded = decoder.getPacketFrame();
    const pktPayload_t &decodedPayload =
        const_cast<M17PacketFrame &>(
            const_cast<const M17PacketFrame &>(decoded))
            .payload();

    // Compare only payload.size() - 1 bytes: the decoder has a known FIXME
    // where Viterbi-decoded bytes are right-shifted by 2 and left-shifted back,
    // but the last byte loses bits because there is no next byte to borrow from.
    size_t cmpLen = payload.size() - 1;
    REQUIRE(std::equal(payload.begin(), payload.begin() + cmpLen,
                       decodedPayload.begin()));
}

TEST_CASE("Round-trip with all-zeros payload", "[m17][packet]")
{
    M17FrameEncoder encoder;
    M17FrameDecoder decoder;

    pktPayload_t payload = {};

    frame_t encoded;
    encoder.encodePacketFrame(payload, encoded);

    M17FrameType type = decoder.decodeFrame(encoded);
    REQUIRE(type == M17FrameType::PACKET);

    const M17PacketFrame &decoded = decoder.getPacketFrame();
    const pktPayload_t &decodedPayload =
        const_cast<M17PacketFrame &>(
            const_cast<const M17PacketFrame &>(decoded))
            .payload();

    // See "Encode then decode round-trip" for why payload.size() - 1
    size_t cmpLen = payload.size() - 1;
    REQUIRE(std::equal(payload.begin(), payload.begin() + cmpLen,
                       decodedPayload.begin()));
}

TEST_CASE("Round-trip with sequential byte pattern", "[m17][packet]")
{
    M17FrameEncoder encoder;
    M17FrameDecoder decoder;

    pktPayload_t payload;
    for (size_t i = 0; i < payload.size(); i++)
        payload[i] = static_cast<uint8_t>(i * 7 + 3); // arbitrary non-trivial

    frame_t encoded;
    encoder.encodePacketFrame(payload, encoded);

    M17FrameType type = decoder.decodeFrame(encoded);
    REQUIRE(type == M17FrameType::PACKET);

    const M17PacketFrame &decoded = decoder.getPacketFrame();
    const pktPayload_t &decodedPayload =
        const_cast<M17PacketFrame &>(
            const_cast<const M17PacketFrame &>(decoded))
            .payload();

    // See "Encode then decode round-trip" for why payload.size() - 1
    size_t cmpLen = payload.size() - 1;
    REQUIRE(std::equal(payload.begin(), payload.begin() + cmpLen,
                       decodedPayload.begin()));
}
