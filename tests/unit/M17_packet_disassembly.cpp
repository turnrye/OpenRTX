/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include "protocols/M17/PacketDisassembler.hpp"
#include "protocols/M17/PacketAssembler.hpp"
#include "core/crc.h"

using namespace M17;

// ==========================================================================
// Reset / initial state
// ==========================================================================

TEST_CASE("PacketDisassembler: fresh instance has zero length",
          "[m17][disassembler]")
{
    PacketDisassembler d;
    REQUIRE(d.length() == 0);
    REQUIRE(d.data() != nullptr);
}

TEST_CASE("PacketDisassembler: reset clears state for reuse",
          "[m17][disassembler]")
{
    PacketDisassembler d;

    // Push a valid single-frame packet, then reset and push another
    uint8_t appData[] = { 0x05, 'A' };
    PacketFrame frames[4];
    size_t n = buildPacketFrames(appData, sizeof(appData), frames, 4);
    REQUIRE(n == 1);

    auto r = d.pushFrame(frames[0]);
    REQUIRE(r == DisassemblerResult::COMPLETE);
    REQUIRE(d.length() == sizeof(appData));

    d.reset();
    REQUIRE(d.length() == 0);

    // Should accept a new packet from frame 0 after reset
    uint8_t appData2[] = { 0x05, 'B', 'C' };
    PacketFrame frames2[4];
    n = buildPacketFrames(appData2, sizeof(appData2), frames2, 4);
    REQUIRE(n == 1);

    r = d.pushFrame(frames2[0]);
    REQUIRE(r == DisassemblerResult::COMPLETE);
    REQUIRE(d.length() == sizeof(appData2));
    REQUIRE(memcmp(d.data(), appData2, sizeof(appData2)) == 0);
}

// ==========================================================================
// Single-frame packets
// ==========================================================================

TEST_CASE("PacketDisassembler: single-frame packet completes with valid CRC",
          "[m17][disassembler]")
{
    PacketDisassembler d;
    uint8_t appData[] = { 0x05, 'H', 'i' };
    PacketFrame frames[4];
    size_t n = buildPacketFrames(appData, sizeof(appData), frames, 4);
    REQUIRE(n == 1);

    auto r = d.pushFrame(frames[0]);
    REQUIRE(r == DisassemblerResult::COMPLETE);
    REQUIRE(d.length() == sizeof(appData));
    REQUIRE(memcmp(d.data(), appData, sizeof(appData)) == 0);
}

TEST_CASE("PacketDisassembler: single-frame preserves all data bytes",
          "[m17][disassembler]")
{
    PacketDisassembler d;

    // Fill with sequential bytes, protocol ID + 10 payload bytes
    uint8_t appData[11];
    for (size_t i = 0; i < sizeof(appData); i++)
        appData[i] = i;

    PacketFrame frames[4];
    size_t n = buildPacketFrames(appData, sizeof(appData), frames, 4);
    REQUIRE(n == 1);

    auto r = d.pushFrame(frames[0]);
    REQUIRE(r == DisassemblerResult::COMPLETE);

    for (size_t i = 0; i < sizeof(appData); i++)
        REQUIRE(d.data()[i] == appData[i]);
}

// ==========================================================================
// Multi-frame packets
// ==========================================================================

TEST_CASE("PacketDisassembler: two-frame packet reassembles correctly",
          "[m17][disassembler]")
{
    PacketDisassembler d;

    // 26 bytes of app data → 28 with CRC → needs 2 frames
    uint8_t appData[26];
    for (size_t i = 0; i < sizeof(appData); i++)
        appData[i] = i;

    PacketFrame frames[4];
    size_t n = buildPacketFrames(appData, sizeof(appData), frames, 4);
    REQUIRE(n == 2);

    auto r1 = d.pushFrame(frames[0]);
    REQUIRE(r1 == DisassemblerResult::IN_PROGRESS);

    auto r2 = d.pushFrame(frames[1]);
    REQUIRE(r2 == DisassemblerResult::COMPLETE);
    REQUIRE(d.length() == sizeof(appData));
    REQUIRE(memcmp(d.data(), appData, sizeof(appData)) == 0);
}

TEST_CASE("PacketDisassembler: three-frame packet with sequential counters",
          "[m17][disassembler]")
{
    PacketDisassembler d;

    // 55 bytes → 57 with CRC → needs 3 frames (25 + 25 + 7)
    uint8_t appData[55];
    for (size_t i = 0; i < sizeof(appData); i++)
        appData[i] = i & 0xFF;

    PacketFrame frames[4];
    size_t n = buildPacketFrames(appData, sizeof(appData), frames, 4);
    REQUIRE(n == 3);

    REQUIRE(d.pushFrame(frames[0]) == DisassemblerResult::IN_PROGRESS);
    REQUIRE(d.pushFrame(frames[1]) == DisassemblerResult::IN_PROGRESS);
    REQUIRE(d.pushFrame(frames[2]) == DisassemblerResult::COMPLETE);
    REQUIRE(d.length() == sizeof(appData));
    REQUIRE(memcmp(d.data(), appData, sizeof(appData)) == 0);
}

// ==========================================================================
// Error cases: sequence errors
// ==========================================================================

TEST_CASE("PacketDisassembler: frame gap returns ERR_SEQUENCE",
          "[m17][disassembler]")
{
    PacketDisassembler d;

    // Build a 2-frame packet
    uint8_t appData[26];
    memset(appData, 0xAA, sizeof(appData));
    PacketFrame frames[4];
    size_t n = buildPacketFrames(appData, sizeof(appData), frames, 4);
    REQUIRE(n == 2);

    // Skip frame 0, push frame 1 as intermediate (wrong counter)
    // Construct a frame with counter=1 but not EOF
    PacketFrame wrongFrame;
    memcpy(wrongFrame.data(), frames[0].data(), PacketFrame::DATA_SIZE);
    wrongFrame.setCounter(1);
    wrongFrame.setEof(false);

    auto r = d.pushFrame(wrongFrame);
    REQUIRE(r == DisassemblerResult::ERR_SEQUENCE);
}

TEST_CASE("PacketDisassembler: duplicate counter returns ERR_SEQUENCE",
          "[m17][disassembler]")
{
    PacketDisassembler d;

    // Build a 3-frame packet
    uint8_t appData[55];
    memset(appData, 0xBB, sizeof(appData));
    PacketFrame frames[4];
    size_t n = buildPacketFrames(appData, sizeof(appData), frames, 4);
    REQUIRE(n == 3);

    REQUIRE(d.pushFrame(frames[0]) == DisassemblerResult::IN_PROGRESS);

    // Push frame 0 again instead of frame 1
    auto r = d.pushFrame(frames[0]);
    REQUIRE(r == DisassemblerResult::ERR_SEQUENCE);
}

// ==========================================================================
// Error cases: CRC corruption
// ==========================================================================

TEST_CASE("PacketDisassembler: corrupted CRC returns ERR_CRC",
          "[m17][disassembler]")
{
    PacketDisassembler d;

    uint8_t appData[] = { 0x05, 'T', 'e', 's', 't' };
    PacketFrame frames[4];
    size_t n = buildPacketFrames(appData, sizeof(appData), frames, 4);
    REQUIRE(n == 1);

    // Corrupt the last EOF frame's data (where CRC sits)
    frames[0][sizeof(appData)] ^= 0xFF;

    auto r = d.pushFrame(frames[0]);
    REQUIRE(r == DisassemblerResult::ERR_CRC);
}

TEST_CASE("PacketDisassembler: corrupted payload data fails CRC",
          "[m17][disassembler]")
{
    PacketDisassembler d;

    uint8_t appData[] = { 0x05, 'H', 'e', 'l', 'l', 'o' };
    PacketFrame frames[4];
    size_t n = buildPacketFrames(appData, sizeof(appData), frames, 4);
    REQUIRE(n == 1);

    // Flip a bit in the payload area
    frames[0][2] ^= 0x01;

    auto r = d.pushFrame(frames[0]);
    REQUIRE(r == DisassemblerResult::ERR_CRC);
}

// ==========================================================================
// Binary data with embedded nulls
// ==========================================================================

TEST_CASE("PacketDisassembler: binary data with embedded NULs",
          "[m17][disassembler]")
{
    PacketDisassembler d;

    uint8_t appData[] = { 0x00, 0x00, 0xFF, 0x00, 0xAB, 0x00 };
    PacketFrame frames[4];
    size_t n = buildPacketFrames(appData, sizeof(appData), frames, 4);
    REQUIRE(n == 1);

    auto r = d.pushFrame(frames[0]);
    REQUIRE(r == DisassemblerResult::COMPLETE);
    REQUIRE(d.length() == sizeof(appData));
    REQUIRE(memcmp(d.data(), appData, sizeof(appData)) == 0);
}

// ==========================================================================
// Exact frame boundary (25 bytes data → 27 with CRC → 2 frames)
// ==========================================================================

TEST_CASE("PacketDisassembler: data at exact 25-byte boundary",
          "[m17][disassembler]")
{
    PacketDisassembler d;

    // 25 bytes of data → 27 with CRC → 2 frames (25 + 2)
    uint8_t appData[25];
    for (size_t i = 0; i < sizeof(appData); i++)
        appData[i] = i + 1;

    PacketFrame frames[4];
    size_t n = buildPacketFrames(appData, sizeof(appData), frames, 4);
    REQUIRE(n == 2);

    REQUIRE(d.pushFrame(frames[0]) == DisassemblerResult::IN_PROGRESS);
    REQUIRE(d.pushFrame(frames[1]) == DisassemblerResult::COMPLETE);
    REQUIRE(d.length() == sizeof(appData));
    REQUIRE(memcmp(d.data(), appData, sizeof(appData)) == 0);
}

// ==========================================================================
// TX → RX round-trip integration tests
// ==========================================================================

TEST_CASE(
    "Round-trip: single-frame SMS through buildPacketFrames + PacketDisassembler",
    "[m17][integration]")
{
    // TX side: build frames
    uint8_t appData[] = { 0x05, 'O', 'p', 'e', 'n', 'R', 'T', 'X', 0x00 };
    PacketFrame frames[4];
    size_t n = buildPacketFrames(appData, sizeof(appData), frames, 4);
    REQUIRE(n > 0);

    // RX side: reassemble
    PacketDisassembler d;
    DisassemblerResult r = DisassemblerResult::IN_PROGRESS;
    for (size_t i = 0; i < n; i++)
        r = d.pushFrame(frames[i]);

    REQUIRE(r == DisassemblerResult::COMPLETE);
    REQUIRE(d.length() == sizeof(appData));
    REQUIRE(memcmp(d.data(), appData, sizeof(appData)) == 0);

    // Verify SMS content at application layer
    REQUIRE(d.data()[0] == 0x05);
    REQUIRE(strcmp((const char *)&d.data()[1], "OpenRTX") == 0);
}

TEST_CASE(
    "Round-trip: multi-frame packet through buildPacketFrames + PacketDisassembler",
    "[m17][integration]")
{
    // Build a 100-byte application payload (protocol ID + 99 bytes)
    uint8_t appData[100];
    appData[0] = 0x05;
    for (size_t i = 1; i < sizeof(appData); i++)
        appData[i] = i & 0xFF;

    PacketFrame frames[8];
    size_t n = buildPacketFrames(appData, sizeof(appData), frames, 8);
    REQUIRE(n > 1); // Should be multi-frame

    PacketDisassembler d;
    DisassemblerResult r = DisassemblerResult::IN_PROGRESS;
    for (size_t i = 0; i < n; i++) {
        r = d.pushFrame(frames[i]);
        if (i < n - 1)
            REQUIRE(r == DisassemblerResult::IN_PROGRESS);
    }

    REQUIRE(r == DisassemblerResult::COMPLETE);
    REQUIRE(d.length() == sizeof(appData));
    REQUIRE(memcmp(d.data(), appData, sizeof(appData)) == 0);
}

TEST_CASE("Round-trip: raw protocol ID 0x00 (non-SMS) round-trips correctly",
          "[m17][integration]")
{
    // Use RAW protocol ID to show disassembler is protocol-agnostic
    uint8_t appData[] = { 0x00, 0xDE, 0xAD, 0xBE, 0xEF };
    PacketFrame frames[4];
    size_t n = buildPacketFrames(appData, sizeof(appData), frames, 4);
    REQUIRE(n == 1);

    PacketDisassembler d;
    auto r = d.pushFrame(frames[0]);
    REQUIRE(r == DisassemblerResult::COMPLETE);
    REQUIRE(d.length() == sizeof(appData));
    REQUIRE(d.data()[0] == 0x00);
    REQUIRE(memcmp(d.data(), appData, sizeof(appData)) == 0);
}

TEST_CASE("Round-trip: max-size packet (823 bytes app data)",
          "[m17][integration]")
{
    // 823 bytes is the M17 spec max for application data
    // 823 + 2 CRC = 825 = 33 * 25 → exactly 33 frames
    uint8_t appData[823];
    for (size_t i = 0; i < sizeof(appData); i++)
        appData[i] = i & 0xFF;
    appData[0] = 0x05;

    PacketFrame frames[33];
    size_t n = buildPacketFrames(appData, sizeof(appData), frames, 33);
    REQUIRE(n == 33);

    PacketDisassembler d;
    DisassemblerResult r = DisassemblerResult::IN_PROGRESS;
    for (size_t i = 0; i < n; i++)
        r = d.pushFrame(frames[i]);

    REQUIRE(r == DisassemblerResult::COMPLETE);
    REQUIRE(d.length() == sizeof(appData));
    REQUIRE(memcmp(d.data(), appData, sizeof(appData)) == 0);
}
