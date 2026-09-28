/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * Characterization tests for the M17 packet deframer on the LOSSY reception
 * path: frame loss, reordering and duplication.
 *
 * The clean round-trip tests in M17_packet_disassembly.cpp only ever feed a
 * complete, in-order frame sequence.  These tests document what the deframer
 * does when the on-air sequence is imperfect, which is what a real receiver
 * sees.  M17 packet mode has NO ARQ: a frame that is genuinely lost cannot be
 * recovered at this layer, so the expected outcome of a dropped frame is that
 * the whole packet fails.  These tests pin that behavior down (and would catch
 * an accidental "half a packet accepted" regression), and characterize the two
 * anomalies that are, in principle, recoverable but are currently fatal:
 * reordering and duplication.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <vector>
#include "protocols/M17/PacketDeframer.hpp"
#include "protocols/M17/PacketFramer.hpp"
#include "core/crc.h"

using namespace M17;

// Shared receive buffer for the deframer under test.
static uint8_t s_rxBuf[M17_MAX_PACKET_DATA];

// Build a deterministic application payload of the requested length: an
// SMS-style protocol-ID byte followed by a repeating letter pattern.
static void makePayload(std::vector<uint8_t> &buf, size_t appLen)
{
    buf.resize(appLen);
    buf[0] = 0x05; // SMS protocol ID
    for (size_t i = 1; i < appLen; i++)
        buf[i] = static_cast<uint8_t>('A' + (i % 26));
}

// Frame an application payload into an ordered list of on-air PacketFrames.
static std::vector<PacketFrame> frameAll(std::vector<uint8_t> &data)
{
    std::vector<PacketFrame> frames;
    PacketFramer framer;
    if (!framer.init(data.data(), data.size()))
        return frames;

    bool last = false;
    while (!last) {
        PacketFrame f;
        last = framer.nextFrame(f);
        frames.push_back(f);
    }
    return frames;
}

// Application length that frames into exactly `n` on-air frames.  The framer
// emits ceil((appLen + 2) / 25) frames; this leaves the final frame roughly
// half full so no edge sits exactly on a 25-byte boundary.
static size_t appLenForFrames(size_t n)
{
    return (n - 1) * PacketFrame::DATA_SIZE + 10;
}

// ==========================================================================
// (a) Baseline: a complete, in-order multi-frame packet reassembles.
// ==========================================================================

TEST_CASE("Deframer loss: complete in-order packet reassembles for 1..8 frames",
          "[m17][packet][deframer]")
{
    for (size_t n = 1; n <= 8; n++) {
        std::vector<uint8_t> app;
        makePayload(app, appLenForFrames(n));
        auto frames = frameAll(app);
        INFO("frame count = " << n);
        REQUIRE(frames.size() == n);

        PacketDeframer d;
        REQUIRE(d.init(s_rxBuf, sizeof(s_rxBuf)));

        DeframerResult r = DeframerResult::IN_PROGRESS;
        for (size_t i = 0; i < frames.size(); i++) {
            r = d.pushFrame(frames[i]);
            if (i + 1 < frames.size())
                REQUIRE(r == DeframerResult::IN_PROGRESS);
        }
        REQUIRE(r == DeframerResult::COMPLETE);
        REQUIRE(d.length() == app.size());
        REQUIRE(memcmp(d.data(), app.data(), app.size()) == 0);
    }
}

// ==========================================================================
// (b) One frame dropped, swept across frame index and packet size.
//
// The invariant that matters for the field bug: dropping ANY single frame of
// a multi-frame packet never yields a completed packet.  There is no partial
// recovery -- one lost frame loses the whole message.  This is exactly why
// longer SMS (more frames) decode worse: with per-frame loss probability the
// whole-packet success rate falls off as (1 - p)^N.
// ==========================================================================

TEST_CASE("Deframer loss: dropping any single frame never completes the packet",
          "[m17][packet][deframer]")
{
    for (size_t n = 2; n <= 6; n++) {
        std::vector<uint8_t> app;
        makePayload(app, appLenForFrames(n));
        auto frames = frameAll(app);
        REQUIRE(frames.size() == n);

        for (size_t drop = 0; drop < n; drop++) {
            INFO("packet frames = " << n << ", dropped index = " << drop);

            PacketDeframer d;
            REQUIRE(d.init(s_rxBuf, sizeof(s_rxBuf)));

            bool sawComplete = false;
            bool sawError = false;
            DeframerResult last = DeframerResult::IN_PROGRESS;
            for (size_t i = 0; i < frames.size(); i++) {
                if (i == drop)
                    continue; // this frame was lost on the air
                last = d.pushFrame(frames[i]);
                if (last == DeframerResult::COMPLETE)
                    sawComplete = true;
                if ((last != DeframerResult::IN_PROGRESS)
                    && (last != DeframerResult::COMPLETE))
                    sawError = true;
            }

            // The packet is never recovered from a single missing frame.
            CHECK_FALSE(sawComplete);

            if (drop == n - 1) {
                // The EOF frame was the one lost.  Every surviving frame is a
                // valid intermediate frame, so the deframer accepts them all
                // and simply waits forever for an end that never comes: no
                // error is raised, the reassembly just stalls IN_PROGRESS.
                // At the OpMode layer this leaves the RX descriptor pinned
                // until the demodulator loses lock and the mode resets it.
                CHECK(last == DeframerResult::IN_PROGRESS);
                CHECK_FALSE(sawError);
            } else {
                // An intermediate frame was lost.  The next frame breaks the
                // sequential-counter contract (ERR_SEQUENCE), or -- when the
                // dropped frame was the last intermediate one -- the EOF frame
                // is accepted against a short buffer and fails CRC (ERR_CRC).
                // Either way the OpMode layer aborts the descriptor (-EIO).
                CHECK(sawError);
                CHECK((last == DeframerResult::ERR_SEQUENCE
                       || last == DeframerResult::ERR_CRC));
            }
        }
    }
}

// ==========================================================================
// (c) Reordered frames.
//
// A serial demodulator does not normally reorder frames, but if it did the
// deframer would treat it as a fatal sequence error rather than tolerating it.
// ==========================================================================

TEST_CASE("Deframer loss: swapping two adjacent frames is fatal",
          "[m17][packet][deframer]")
{
    std::vector<uint8_t> app;
    makePayload(app, appLenForFrames(4));
    auto frames = frameAll(app);
    REQUIRE(frames.size() == 4);

    PacketDeframer d;
    REQUIRE(d.init(s_rxBuf, sizeof(s_rxBuf)));

    // Deliver frame 1 before frame 0.
    CHECK(d.pushFrame(frames[1]) == DeframerResult::ERR_SEQUENCE);
}

TEST_CASE("Deframer loss: reordering two middle frames is fatal",
          "[m17][packet][deframer]")
{
    std::vector<uint8_t> app;
    makePayload(app, appLenForFrames(5));
    auto frames = frameAll(app);
    REQUIRE(frames.size() == 5);

    PacketDeframer d;
    REQUIRE(d.init(s_rxBuf, sizeof(s_rxBuf)));

    // Order 0, 2, 1, ... : frame 2 arrives where frame 1 is expected.
    CHECK(d.pushFrame(frames[0]) == DeframerResult::IN_PROGRESS);
    CHECK(d.pushFrame(frames[2]) == DeframerResult::ERR_SEQUENCE);
}

// ==========================================================================
// (d) Duplicated frames.
//
// A re-delivered frame carries no new information: an ideal deframer could
// ignore it and keep going.  The current implementation instead treats the
// duplicate as an out-of-sequence frame and destroys the whole packet.  This
// is UNNECESSARY loss (unlike a genuinely missing frame), documented here so
// that a future "tolerate duplicates" change has a red-to-green anchor.
// ==========================================================================

TEST_CASE("Deframer loss: an immediately duplicated frame is fatal",
          "[m17][packet][deframer]")
{
    std::vector<uint8_t> app;
    makePayload(app, appLenForFrames(3));
    auto frames = frameAll(app);
    REQUIRE(frames.size() == 3);

    PacketDeframer d;
    REQUIRE(d.init(s_rxBuf, sizeof(s_rxBuf)));

    CHECK(d.pushFrame(frames[0]) == DeframerResult::IN_PROGRESS);
    // Frame 0 arrives a second time (counter already consumed).
    CHECK(d.pushFrame(frames[0]) == DeframerResult::ERR_SEQUENCE);
}

TEST_CASE("Deframer loss: a duplicated mid-stream frame is fatal",
          "[m17][packet][deframer]")
{
    std::vector<uint8_t> app;
    makePayload(app, appLenForFrames(4));
    auto frames = frameAll(app);
    REQUIRE(frames.size() == 4);

    PacketDeframer d;
    REQUIRE(d.init(s_rxBuf, sizeof(s_rxBuf)));

    CHECK(d.pushFrame(frames[0]) == DeframerResult::IN_PROGRESS);
    CHECK(d.pushFrame(frames[1]) == DeframerResult::IN_PROGRESS);
    // Frame 1 repeats: an ideal deframer would ignore it and still complete.
    CHECK(d.pushFrame(frames[1]) == DeframerResult::ERR_SEQUENCE);
}
