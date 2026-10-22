/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Scenarios for the HR_C6000 DMR driver built for the MD-UV3x0, run against
 * the chip model. This radio follows the TYT firmware where it differs from
 * the CS7000 Plus: its chip setup and receiver re-arm, a slot judged on its
 * colour code as soon as the timeslot interrupt comes, the vocoder registers
 * left alone while receiving, and the DMR mode switched for transmission and
 * set up again after it.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>
#include "scenario.h"

/* The TYT direct mode setup: mode, interrupt masks, baseband receive. */
static const std::vector<RegWrite> TYT_MODE = { { 0x10, 0x6B },
                                                { 0x81, 0x19 },
                                                { 0x85, 0x00 },
                                                { 0xA1, 0x80 } };

/* The TYT receiver re-arm: 40=03 41=20 41=00 40=43 41=40 83=FF. */
static const std::vector<RegWrite> TYT_ARM = { { 0x40, 0x03 }, { 0x41, 0x20 },
                                               { 0x41, 0x00 }, { 0x40, 0x43 },
                                               { 0x41, 0x40 }, { 0x83, 0xFF } };

static bool armedIn(const std::vector<Write> &ws)
{
    return containsSequence(configWrites(ws, { 0x40, 0x41, 0x83 }), TYT_ARM);
}

/* A slot boundary, the slot reading the given colour code. */
static void slot(uint8_t cc)
{
    setReg(CONFIG, 0x52, cc << 4);
    timeslot();
}

/* Start a call: a slot with the colour code, then a header burst. */
static void startCall(uint32_t dst = 9, uint32_t src = 3101003)
{
    slot(CC);
    setLinkControl(groupLc(dst, src));
    burst(HEADER, CC);
}

TEST_CASE("MD-UV3x0 DMR sets direct mode up the way the TYT firmware does",
          "[dmr][harness][uv3x0]")
{
    Driver d;
    std::vector<Write> ws = writes();

    REQUIRE(containsSequence(configWrites(ws, { 0x10, 0x81, 0x85, 0xA1 }),
                             TYT_MODE));
    REQUIRE(lastWrite(CONFIG, 0x5F) == 0xC0);
    REQUIRE(lastWrite(CONFIG, 0x1F) == (CC << 4));

    // The MS sourced data sync pattern
    std::vector<uint8_t> sync;
    for (uint16_t a = 0x04; a <= 0x09; a++)
        sync.push_back(lastWrite(AUX, a));
    REQUIRE(sync
            == std::vector<uint8_t>({ 0xD5, 0xD7, 0xF7, 0x7F, 0xD7, 0x57 }));

    // No DMR layer reset and no vocoder register writes, unlike the CS7000 Plus
    REQUIRE_FALSE(wroteAny(ws, { 0x00, 0x20, 0x21, 0x22, 0x64 }));

    REQUIRE(armedIn(ws));
}

TEST_CASE("MD-UV3x0 DMR reads the slot as soon as the interrupt comes",
          "[dmr][harness][uv3x0]")
{
    Driver d;
    long long t = now();
    slot(CC);
    REQUIRE(now() == t);
    REQUIRE(lastWrite(CONFIG, 0x41) == 0x40);
}

TEST_CASE("MD-UV3x0 DMR re-arms after eight slots without its colour code",
          "[dmr][harness][uv3x0]")
{
    Driver d;
    slot(CC);
    clearWrites();

    for (int n = 0; n < 7; n++)
        slot(5);
    REQUIRE_FALSE(armedIn(writes()));

    // A slot with the colour code starts the count again
    slot(CC);
    for (int n = 0; n < 7; n++)
        slot(5);
    REQUIRE_FALSE(armedIn(writes()));

    slot(5); // The eighth in a row
    REQUIRE(armedIn(writes()));
}

TEST_CASE("MD-UV3x0 DMR takes bursts from the first slot boundary on",
          "[dmr][harness][uv3x0]")
{
    Driver d;

    // Before any slot boundary the receiver is not aligned yet
    setLinkControl(groupLc(9, 3101003));
    burst(HEADER, CC);
    REQUIRE_FALSE(poll().valid);

    slot(CC);
    burst(HEADER, CC);
    RxState rx = poll();
    REQUIRE(rx.valid);
    REQUIRE(rx.dstId == 9);
    REQUIRE(rx.srcId == 3101003);
}

TEST_CASE("MD-UV3x0 DMR plays voice whatever colour code its burst reports",
          "[dmr][harness][uv3x0]")
{
    Driver d;
    startCall();

    // The colour code is judged on the slots, not on each voice burst
    setPayload(voicePayload());
    burst(FRAME_B, 7);
    REQUIRE(audioFramesPushed() == 3);
}

TEST_CASE("MD-UV3x0 DMR leaves the vocoder registers alone while receiving",
          "[dmr][harness][uv3x0]")
{
    Driver d;
    startCall();
    setPayload(voicePayload());
    burst(FRAME_A, CC);
    burst(FRAME_B, CC);
    burst(TERMINATOR, CC);
    REQUIRE_FALSE(poll().valid);

    REQUIRE_FALSE(wroteAny(writes(), { 0x20, 0x21, 0x22 }));
}

TEST_CASE("MD-UV3x0 DMR transmits in mode 0x6C and sets direct mode up after",
          "[dmr][harness][uv3x0]")
{
    Driver d;
    clearWrites();
    keyUp(true, 9, 3101003);

    // Key-up: the transmit mode before the transmit timing
    REQUIRE(containsSequence(configWrites(writes(), { 0x10, 0x40 }),
                             { { 0x10, 0x6C }, { 0x40, 0xA3 } }));
    REQUIRE(txActive());

    // Run the transmission to its end
    unkey();
    int n = 0;
    while (txActive() && n < 60) {
        setTime(30 * (n + 1));
        timeslot();
        n++;
    }
    REQUIRE_FALSE(txActive());

    // The receiver comes back with the front end, direct mode and colour code
    // set up again, then the re-arm
    clearWrites();
    timeslot();
    std::vector<Write> ws = writes();
    REQUIRE(lastWrite(CONFIG, 0x01) == 0xF0); // Front end
    REQUIRE(containsSequence(configWrites(ws, { 0x10, 0x81, 0x85, 0xA1 }),
                             TYT_MODE));
    REQUIRE(lastWrite(CONFIG, 0x1F) == (CC << 4));
    REQUIRE(armedIn(ws));
}
