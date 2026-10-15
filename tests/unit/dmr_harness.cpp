/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Scenarios for the HR_C6000 DMR driver, run against the chip model: the
 * rules found on the bench, with the stock firmware as the reference, that a
 * change to the driver must keep.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>
#include "harness.h"

using namespace harness;
using Bytes = std::vector<uint8_t>;

/* Register pages on the bus. */
static const uint8_t AUX = 1, DATA = 2, CONFIG = 4;

/* Burst status: sync class in bits 1:0, data type or frame number above. */
static const uint8_t HEADER = 0x12; // Voice LC header
static const uint8_t TERMINATOR = 0x22;
static const uint8_t FRAME_A = 0x11;
static const uint8_t FRAME_B = 0x20;
static const uint8_t EMPTY = 0x00; // Class 0 with nothing decoded
static const uint8_t NOISE = 0x03; // Class 3: nothing recognisable

/* Interrupt flags of register 0x82. */
static const uint8_t FLAG_BURST = 0x08, FLAG_LC = 0x10, FLAG_TICK = 0x20;

static const uint8_t CC = 1;

/* The stock re-arm sequence: 41=00 40=03 41=20 41=40 40=43. */
static bool rearmSequenceIn(const std::vector<Write> &ws, size_t from = 0)
{
    const std::vector<std::pair<uint16_t, uint8_t>> seq = { { 0x41, 0x00 },
                                                            { 0x40, 0x03 },
                                                            { 0x41, 0x20 },
                                                            { 0x41, 0x40 },
                                                            { 0x40, 0x43 } };
    size_t k = 0;
    for (size_t i = from; i < ws.size(); i++) {
        if (ws[i].page != CONFIG)
            continue;
        if (ws[i].addr == seq[k].first && ws[i].value == seq[k].second) {
            if (++k == seq.size())
                return true;
        } else if (ws[i].addr == 0x40 || ws[i].addr == 0x41)
            k = (ws[i].addr == 0x41 && ws[i].value == 0x00) ? 1 : 0;
    }
    return false;
}

static Bytes groupLc(uint32_t dst, uint32_t src)
{
    return { 0x00,
             0x00,
             0x00,
             (uint8_t)(dst >> 16),
             (uint8_t)(dst >> 8),
             (uint8_t)dst,
             (uint8_t)(src >> 16),
             (uint8_t)(src >> 8),
             (uint8_t)src,
             0x00,
             0x00,
             0x00 };
}

/* A voice frame received over the air, three times over: a voice payload. */
static Bytes voicePayload()
{
    Bytes f = { 0xB9, 0xCE, 0xA3, 0x76, 0x8F, 0xBB, 0x69, 0xCA, 0xCD };
    Bytes p;
    for (int i = 0; i < 3; i++)
        p.insert(p.end(), f.begin(), f.end());
    return p;
}

/* Bring the receiver to locked, as a header with the right colour code does
 * at the slot handler. */
static void lockOnHeader()
{
    setReg(CONFIG, 0x51, HEADER);
    setReg(CONFIG, 0x52, CC << 4);
    timeslot();
}

/* Start a call: lock, then the header burst with its link control. */
static void startCall(uint32_t dst = 9, uint32_t src = 3101003)
{
    lockOnHeader();
    setLinkControl(groupLc(dst, src));
    burst(HEADER, CC);
}

struct Driver {
    Driver(uint8_t cc = CC)
    {
        reset();
        start(cc);
    }
    ~Driver()
    {
        stop();
        trace();
    }
};

TEST_CASE("DMR driver arms the receiver as the stock firmware does",
          "[dmr][harness]")
{
    Driver d;
    auto ws = writes();
    REQUIRE(rearmSequenceIn(ws));
    auto cc = writes(CONFIG, 0x1F);
    REQUIRE_FALSE(cc.empty());
    REQUIRE(cc.back().value == (CC << 4));
}

TEST_CASE("DMR call starts from a header and ends on its terminator",
          "[dmr][harness]")
{
    Driver d;
    startCall();

    RxState rx = poll();
    REQUIRE(rx.valid);
    REQUIRE(rx.groupCall);
    REQUIRE(rx.dstId == 9);
    REQUIRE(rx.srcId == 3101003);
    REQUIRE(rx.colorCode == CC);
    REQUIRE(audioRxStarts() == 1);
    // The decode buffer is cleared when a call is accepted
    REQUIRE_FALSE(writes(CONFIG, 0x21).empty());

    clearWrites();
    burst(TERMINATOR, CC);
    rx = poll();
    REQUIRE_FALSE(rx.valid);
    REQUIRE(audioRxStops() == 1);

    // Stock call end: 22=16, re-arm, 20=00, 21=01
    auto ws = writes();
    size_t i22 = 0;
    for (; i22 < ws.size(); i22++)
        if (ws[i22].page == CONFIG && ws[i22].addr == 0x22
            && ws[i22].value == 0x16)
            break;
    REQUIRE(i22 < ws.size());
    REQUIRE(rearmSequenceIn(ws, i22));
    REQUIRE(writes(CONFIG, 0x20).back().value == 0x00);
    REQUIRE(writes(CONFIG, 0x21).back().value == 0x01);
}

TEST_CASE("DMR voice reaches playback, except from the wrong colour code",
          "[dmr][harness]")
{
    Driver d;
    startCall();
    setPayload(voicePayload());

    burst(FRAME_B, CC);
    REQUIRE(audioFramesPushed() == 3);

    // Frame A is taken whatever the colour code register says
    setPayload(voicePayload());
    setPayload({ 0xDF, 0x88, 0x82, 0x76, 0x86, 0x84, 0x5E, 0x6F, 0xC1,
                 0xDF, 0x88, 0x82, 0x76, 0x86, 0x84, 0x5E, 0x6F, 0xC1,
                 0xDF, 0x88, 0x82, 0x76, 0x86, 0x84, 0x5E, 0x6F, 0xC1 });
    burst(FRAME_A, 7);
    REQUIRE(audioFramesPushed() == 6);

    // The idle slot's noise carries a random colour code: dropped
    setPayload({ 0xCC, 0x88, 0xA0, 0x54, 0xC0, 0xC5, 0x3C, 0x6F, 0x81,
                 0xCC, 0x88, 0xA0, 0x54, 0xC0, 0xC5, 0x3C, 0x6F, 0x81,
                 0xCC, 0x88, 0xA0, 0x54, 0xC0, 0xC5, 0x3C, 0x6F, 0x81 });
    burst(FRAME_B, 7);
    REQUIRE(audioFramesPushed() == 6);
}

TEST_CASE("DMR slot ticks alone neither start nor hold a call",
          "[dmr][harness]")
{
    Driver d;
    lockOnHeader();
    setLinkControl(groupLc(9, 3101003));

    // The chip still holds the previous link control: a tick must not read it
    for (int n = 0; n < 20; n++)
        burst(HEADER, CC, FLAG_TICK);
    REQUIRE_FALSE(poll().valid);
    REQUIRE(audioRxStarts() == 0);
}

TEST_CASE("DMR bursts are ignored until the receiver has locked",
          "[dmr][harness]")
{
    Driver d;
    setLinkControl(groupLc(9, 3101003));
    burst(HEADER, CC); // No slot handler lock yet
    REQUIRE_FALSE(poll().valid);
    // The flag is still acknowledged
    REQUIRE_FALSE(writes(CONFIG, 0x83).empty());
}

TEST_CASE("DMR header with another colour code is ignored", "[dmr][harness]")
{
    Driver d;
    lockOnHeader();
    setLinkControl(groupLc(9, 3101003));
    burst(HEADER, CC + 1);
    REQUIRE_FALSE(poll().valid);
}

TEST_CASE("DMR lock drops after four bad slots before a call", "[dmr][harness]")
{
    Driver d;
    lockOnHeader();
    clearWrites();

    // While locked every slot asks for the next one
    setReg(CONFIG, 0x51, FRAME_B);
    setReg(CONFIG, 0x52, CC << 4);
    timeslot();
    REQUIRE(writes(CONFIG, 0x41).back().value == 0x40);

    // The carrier goes: noise on every slot
    setReg(CONFIG, 0x51, NOISE);
    setReg(CONFIG, 0x52, 0x50);
    for (int n = 0; n < 3; n++)
        timeslot();
    REQUIRE_FALSE(rearmSequenceIn(writes()));
    clearWrites();
    timeslot(); // The fourth
    REQUIRE(rearmSequenceIn(writes()));
}

TEST_CASE("DMR call holds across superframes whatever the other slots read",
          "[dmr][harness]")
{
    Driver d;
    startCall();
    REQUIRE(poll().valid);

    // Five superframes: frame A on one slot in twelve, noise on the rest
    for (int n = 0; n < 60; n++) {
        setReg(CONFIG, 0x51, (n % 12 == 0) ? FRAME_A : NOISE);
        setReg(CONFIG, 0x52, 0x50);
        timeslot();
    }
    REQUIRE(poll().valid);
}

TEST_CASE("DMR call is lost after 25 slots without a voice sync",
          "[dmr][harness]")
{
    Driver d;
    startCall();
    REQUIRE(poll().valid);
    clearWrites();

    setReg(CONFIG, 0x51, FRAME_B);
    setReg(CONFIG, 0x52, CC << 4);
    for (int n = 0; n < 24; n++)
        timeslot();
    REQUIRE(poll().valid);
    REQUIRE_FALSE(rearmSequenceIn(writes()));

    timeslot(); // The 25th
    REQUIRE_FALSE(poll().valid);
    REQUIRE(rearmSequenceIn(writes()));
    REQUIRE(audioRxStops() == 1);
}

/* The slot action written at each slot boundary from now on. */
static std::vector<uint8_t> slotActions()
{
    std::vector<uint8_t> v;
    for (const Write &w : writes(CONFIG, 0x41))
        v.push_back(w.value);
    return v;
}

TEST_CASE("DMR call receives only its own slot", "[dmr][harness]")
{
    Driver d;
    startCall(); // The header was read at the last slot boundary
    clearWrites();

    // The boundaries alternate between reading the other slot and the call's
    for (int n = 0; n < 3; n++) {
        setReg(CONFIG, 0x51, NOISE);
        setReg(CONFIG, 0x52, 0x50);
        timeslot();
        setReg(CONFIG, 0x51, FRAME_B);
        setReg(CONFIG, 0x52, CC << 4);
        timeslot();
    }

    std::vector<uint8_t> expected = { 0x00, 0x50, 0x00, 0x50, 0x00, 0x50 };
    REQUIRE(slotActions() == expected);
}

TEST_CASE("DMR repeater idle bursts do not take the call's slot",
          "[dmr][harness]")
{
    // A repeater's downlink as received on the bench from W4BS: the call's
    // voice on one slot, idle data bursts with the same colour code on the
    // other, which in direct mode read as data class bursts of type 0.
    static const uint8_t IDLE = 0x02;

    Driver d;
    setReg(CONFIG, 0x52, CC << 4);

    // Locked on an idle burst, then a superframe of the two slots
    setReg(CONFIG, 0x51, IDLE);
    timeslot();
    for (int n = 0; n < 12; n++) {
        if ((n % 2) == 0)
            setReg(CONFIG, 0x51, (n == 0) ? FRAME_A : FRAME_B);
        else
            setReg(CONFIG, 0x51, IDLE);
        timeslot();
    }

    // Late entry: the call's embedded link control, on its voice slot
    setLinkControl(groupLc(31471, 3147073));
    setReg(CONFIG, 0x51, FRAME_B);
    raiseFlags(FLAG_LC);
    system();
    REQUIRE(poll().valid);
    clearWrites();

    // From now on only the voice slot is received
    for (int n = 0; n < 3; n++) {
        setReg(CONFIG, 0x51, FRAME_B);
        timeslot();
        setReg(CONFIG, 0x51, IDLE);
        timeslot();
    }

    std::vector<uint8_t> expected = { 0x50, 0x00, 0x50, 0x00, 0x50, 0x00 };
    REQUIRE(slotActions() == expected);
}

TEST_CASE("DMR call slot survives a slot read late", "[dmr][harness]")
{
    Driver d;
    startCall();
    clearWrites();

    setReg(CONFIG, 0x51, NOISE);
    timeslot();  // The other slot
    timeslot(2); // Two boundaries, one read: the other slot again
    setReg(CONFIG, 0x51, FRAME_B);
    timeslot();  // The call's slot

    std::vector<uint8_t> expected = { 0x00, 0x00, 0x50 };
    REQUIRE(slotActions() == expected);
}

TEST_CASE("DMR slot is read straight away during a call", "[dmr][harness]")
{
    Driver d;
    lockOnHeader();

    // Before a call the slot is read 10ms after the interrupt
    long long t = now();
    timeslot();
    REQUIRE(now() - t >= 10);

    setLinkControl(groupLc(9, 3101003));
    burst(HEADER, CC);
    REQUIRE(poll().valid);

    t = now();
    setReg(CONFIG, 0x51, FRAME_A);
    timeslot();
    REQUIRE(now() == t);
}

TEST_CASE("DMR search restarts after twelve empty slots", "[dmr][harness]")
{
    Driver d;
    clearWrites();
    setReg(CONFIG, 0x51, EMPTY);
    setReg(CONFIG, 0x52, 0x00);
    for (int n = 0; n < 11; n++) {
        timeslot();
        // Unlocked: no per-slot register writes
        REQUIRE(writes(CONFIG, 0x41).empty());
    }
    timeslot();
    REQUIRE(rearmSequenceIn(writes()));
}

TEST_CASE("DMR late entry from the embedded link control", "[dmr][harness]")
{
    Driver d;
    // Locked on a voice frame A, no header seen
    setReg(CONFIG, 0x51, FRAME_A);
    setReg(CONFIG, 0x52, CC << 4);
    timeslot();
    REQUIRE_FALSE(poll().valid);

    setLinkControl(groupLc(91, 2357322));
    setReg(CONFIG, 0x51, FRAME_B);
    setReg(CONFIG, 0x52, CC << 4);
    raiseFlags(FLAG_LC);
    system();

    RxState rx = poll();
    REQUIRE(rx.valid);
    REQUIRE(rx.dstId == 91);
    REQUIRE(rx.srcId == 2357322);
}

TEST_CASE("DMR stale link control does not bring a call back", "[dmr][harness]")
{
    Driver d;
    startCall();
    burst(TERMINATOR, CC);
    REQUIRE_FALSE(poll().valid);

    // After the terminator the chip keeps raising bursts on noise for a
    // while, with the old link control still on its data page
    for (int n = 0; n < 30; n++)
        burst(NOISE, 0x0A, FLAG_BURST);
    REQUIRE_FALSE(poll().valid);
    REQUIRE(audioRxStarts() == 1);
}

TEST_CASE("DMR transmission follows the stock slot sequence", "[dmr][harness]")
{
    Driver d;
    clearWrites();
    keyUp(true, 9, 3101003);

    // Key-up: own ID, 40=A3, 41=40
    REQUIRE(writes(CONFIG, 0x14).back().value == (3101003 & 0xFF));
    REQUIRE(writes(CONFIG, 0x40).back().value == 0xA3);
    REQUIRE(writes(CONFIG, 0x41).back().value == 0x40);
    REQUIRE(audioTxStarts() == 1);
    REQUIRE(txActive());

    // Three headers on alternate slots, the last starting the encoder
    std::vector<uint8_t> burstTypes;
    std::vector<uint8_t> slotActions;
    std::vector<int> encodeStarts; // Slots writing 0x22 = 0x86
    for (int n = 0; n < 6; n++) {
        clearWrites();
        setTime(30 * (n + 1));
        timeslot();
        for (const auto &w : writes()) {
            if (w.page == CONFIG && w.addr == 0x41)
                slotActions.push_back(w.value);
            if (w.page == CONFIG && w.addr == 0x50)
                burstTypes.push_back(w.value);
            if (w.page == CONFIG && w.addr == 0x22 && w.value == 0x86)
                encodeStarts.push_back(n);
        }
    }
    REQUIRE(slotActions
            == std::vector<uint8_t>({ 0x80, 0x00, 0x80, 0x00, 0x80, 0x00 }));
    REQUIRE(burstTypes == std::vector<uint8_t>({ 0x10, 0x10, 0x10 }));
    REQUIRE(encodeStarts == std::vector<int>({ 4 })); // With the third header
    REQUIRE(payloadsWritten().empty());               // No voice yet

    // Voice A..F, each payload loaded in the burst's own slot
    burstTypes.clear();
    for (int n = 6; n < 18; n++) {
        clearWrites();
        setTime(30 * (n + 1));
        timeslot();
        for (const auto &w : writes())
            if (w.page == CONFIG && w.addr == 0x50)
                burstTypes.push_back(w.value);
    }
    REQUIRE(burstTypes
            == std::vector<uint8_t>({ 0x08, 0x18, 0x28, 0x38, 0x48, 0x58 }));
    REQUIRE(payloadsWritten().size() == 6);

    // Unkey mid superframe: voice continues to frame F, then the terminator,
    // an idle slot, and the end
    unkey();
    burstTypes.clear();
    int n = 18;
    while (txActive() && n < 60) {
        clearWrites();
        setTime(30 * (n + 1));
        timeslot();
        for (const auto &w : writes())
            if (w.page == CONFIG && w.addr == 0x50)
                burstTypes.push_back(w.value);
        n++;
    }
    REQUIRE_FALSE(txActive());
    REQUIRE(burstTypes.back() == 0x20);
    for (size_t i = 0; i + 1 < burstTypes.size(); i++)
        REQUIRE(burstTypes[i] != 0x20);
    // The end: 22=46, 21=03, 41=40
    REQUIRE(writes(CONFIG, 0x22).back().value == 0x46);
    REQUIRE(writes(CONFIG, 0x21).back().value == 0x03);
    REQUIRE(writes(CONFIG, 0x41).back().value == 0x40);

    // The receiver comes back on the next event, with the stock sequence
    clearWrites();
    timeslot();
    REQUIRE(rearmSequenceIn(writes()));
    REQUIRE(writes(CONFIG, 0x21).front().value == 0x02);
}

TEST_CASE("DMR sent slots never follow each other", "[dmr][harness]")
{
    Driver d;
    keyUp(true, 9, 3101003);
    uint8_t last = 0x00;
    for (int n = 0; n < 40; n++) {
        clearWrites();
        setTime(30 * (n + 1));
        timeslot();
        auto sa = writes(CONFIG, 0x41);
        if (sa.empty())
            continue;
        REQUIRE_FALSE(((sa.back().value == 0x80) && (last == 0x80)));
        last = sa.back().value;
    }
}

TEST_CASE("DMR transmission winds down on its own if the slot clock stops",
          "[dmr][harness]")
{
    Driver d;
    keyUp(true, 9, 3101003);
    unkey();
    advance(1600);
    txTask(); // The rtx layer's periodic call wakes the thread
    waitIdle();
    REQUIRE_FALSE(txActive());
}
