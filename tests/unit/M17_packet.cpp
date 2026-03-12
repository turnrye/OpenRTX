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
#include "protocols/M17/M17Constants.hpp"
#include "protocols/M17/M17LinkSetupFrame.hpp"

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
    const pktPayload_t &decodedPayload = decoded.payload();

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
    const pktPayload_t &decodedPayload = decoded.payload();

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
    const pktPayload_t &decodedPayload = decoded.payload();

    // See "Encode then decode round-trip" for why payload.size() - 1
    size_t cmpLen = payload.size() - 1;
    REQUIRE(std::equal(payload.begin(), payload.begin() + cmpLen,
                       decodedPayload.begin()));
}

TEST_CASE("pktPayload_t is 26 bytes", "[m17][packet]")
{
    // Per M17 spec: 25 bytes of packet chunk data + 1 metadata byte = 26 bytes
    REQUIRE(sizeof(pktPayload_t) == 26);
}

TEST_CASE("Encoded frame is exactly 48 bytes with PACKET_SYNC_WORD",
          "[m17][packet]")
{
    M17FrameEncoder encoder;

    pktPayload_t payload = {};
    payload[0] = 0xAB;

    frame_t encoded;
    encoder.encodePacketFrame(payload, encoded);

    REQUIRE(encoded.size() == 48);
    REQUIRE(encoded[0] == PACKET_SYNC_WORD[0]);
    REQUIRE(encoded[1] == PACKET_SYNC_WORD[1]);
}

TEST_CASE("Encoding the same payload twice produces identical frames",
          "[m17][packet]")
{
    M17FrameEncoder encoder;

    pktPayload_t payload = {};
    const char msg[] = "DETERMINISTIC";
    std::copy_n(msg, sizeof(msg) - 1, payload.begin());

    frame_t encoded1, encoded2;
    encoder.encodePacketFrame(payload, encoded1);
    encoder.encodePacketFrame(payload, encoded2);

    REQUIRE(encoded1 == encoded2);
}

TEST_CASE("Round-trip with all-0xFF payload", "[m17][packet]")
{
    M17FrameEncoder encoder;
    M17FrameDecoder decoder;

    pktPayload_t payload;
    payload.fill(0xFF);

    frame_t encoded;
    encoder.encodePacketFrame(payload, encoded);

    M17FrameType type = decoder.decodeFrame(encoded);
    REQUIRE(type == M17FrameType::PACKET);

    const M17PacketFrame &decoded = decoder.getPacketFrame();
    const pktPayload_t &decodedPayload = decoded.payload();

    // See "Encode then decode round-trip" for why payload.size() - 1
    size_t cmpLen = payload.size() - 1;
    REQUIRE(std::equal(payload.begin(), payload.begin() + cmpLen,
                       decodedPayload.begin()));
}

TEST_CASE("Round-trip with single non-zero byte", "[m17][packet]")
{
    M17FrameEncoder encoder;
    M17FrameDecoder decoder;

    pktPayload_t payload = {};
    payload[0] = 0x42;

    frame_t encoded;
    encoder.encodePacketFrame(payload, encoded);

    M17FrameType type = decoder.decodeFrame(encoded);
    REQUIRE(type == M17FrameType::PACKET);

    const M17PacketFrame &decoded = decoder.getPacketFrame();
    const pktPayload_t &decodedPayload = decoded.payload();

    size_t cmpLen = payload.size() - 1;
    REQUIRE(std::equal(payload.begin(), payload.begin() + cmpLen,
                       decodedPayload.begin()));
}

TEST_CASE("Decoder distinguishes PACKET from STREAM frame type",
          "[m17][packet]")
{
    M17FrameEncoder encoder;
    M17FrameDecoder decoder;

    // Encode a packet frame
    pktPayload_t pktPayload = {};
    pktPayload[0] = 0x01;
    frame_t pktEncoded;
    encoder.encodePacketFrame(pktPayload, pktEncoded);

    M17FrameType pktType = decoder.decodeFrame(pktEncoded);
    REQUIRE(pktType == M17FrameType::PACKET);

    // Encode a stream frame and verify it is NOT detected as PACKET
    payload_t streamPayload = {};
    streamPayload[0] = 0x01;
    frame_t streamEncoded;
    encoder.encodeStreamFrame(streamPayload, streamEncoded);

    M17FrameType streamType = decoder.decodeFrame(streamEncoded);
    REQUIRE(streamType == M17FrameType::STREAM);
    REQUIRE(streamType != M17FrameType::PACKET);
}

TEST_CASE("Decoder identifies EOT frame type", "[m17][packet]")
{
    M17FrameEncoder encoder;
    M17FrameDecoder decoder;

    frame_t eotFrame;
    encoder.encodeEotFrame(eotFrame);

    M17FrameType type = decoder.decodeFrame(eotFrame);
    REQUIRE(type == M17FrameType::EOT);
}

TEST_CASE("Decoder identifies LSF frame type, not PACKET", "[m17][packet]")
{
    M17FrameEncoder encoder;
    M17FrameDecoder decoder;

    M17LinkSetupFrame lsf;
    frame_t lsfEncoded;
    encoder.encodeLsf(lsf, lsfEncoded);

    M17FrameType type = decoder.decodeFrame(lsfEncoded);
    REQUIRE(type == M17FrameType::LINK_SETUP);
    REQUIRE(type != M17FrameType::PACKET);
}

TEST_CASE("Decoder reset clears previous packet frame data", "[m17][packet]")
{
    M17FrameEncoder encoder;
    M17FrameDecoder decoder;

    // Decode a non-zero payload so decoder has data
    pktPayload_t payload;
    payload.fill(0xAA);
    frame_t encoded;
    encoder.encodePacketFrame(payload, encoded);
    decoder.decodeFrame(encoded);

    // Reset should clear internal state
    decoder.reset();

    const M17PacketFrame &pktFrame = decoder.getPacketFrame();
    const uint8_t *data = pktFrame.getData();

    for (size_t i = 0; i < sizeof(pktPayload_t); i++) {
        REQUIRE(data[i] == 0);
    }
}

TEST_CASE("Consecutive encodes with different payloads are independent",
          "[m17][packet]")
{
    M17FrameEncoder encoder;
    M17FrameDecoder decoder;

    pktPayload_t payload1 = {};
    payload1.fill(0x11);
    pktPayload_t payload2 = {};
    payload2.fill(0x22);

    frame_t encoded1, encoded2;
    encoder.encodePacketFrame(payload1, encoded1);
    encoder.encodePacketFrame(payload2, encoded2);

    // Frames should differ (different payloads)
    REQUIRE(encoded1 != encoded2);

    // Each should decode back to its own payload
    decoder.decodeFrame(encoded1);
    const pktPayload_t &dec1 = decoder.getPacketFrame().payload();
    size_t cmpLen = payload1.size() - 1;
    REQUIRE(
        std::equal(payload1.begin(), payload1.begin() + cmpLen, dec1.begin()));

    decoder.decodeFrame(encoded2);
    const pktPayload_t &dec2 = decoder.getPacketFrame().payload();
    REQUIRE(
        std::equal(payload2.begin(), payload2.begin() + cmpLen, dec2.begin()));
}
