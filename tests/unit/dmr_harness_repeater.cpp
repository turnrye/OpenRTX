/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Scenarios for the HR_C6000 DMR driver on the CS7000 Plus receiving and
 * transmitting through a repeater, against the chip model fed a repeater's
 * downlink: both timeslots back to back, base station sourced, the CACH's TC
 * bit alternating. The driver follows the Connect Systems firmware: it aligns
 * on the TC bits, then receives its timeslot only, keeps the alignment through
 * a call's end, and searches again once the downlink is lost. It transmits on
 * the downlink's timing, on its timeslot only, and only once aligned.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include "scenario.h"

/* An idle burst, what a repeater sends on a timeslot with no call. */
static const uint8_t IDLE_BURST = 0x92;

/* The Connect Systems receiver re-arm. */
static const std::vector<RegWrite> REARM = { { 0x41, 0x00 },
                                             { 0x40, 0x03 },
                                             { 0x41, 0x20 },
                                             { 0x41, 0x40 },
                                             { 0x40, 0x43 } };

static bool rearmedIn(const std::vector<Write> &ws)
{
    return containsSequence(configWrites(ws, { 0x40, 0x41 }), REARM);
}

static int logged(const std::string &text)
{
    int n = 0;
    for (const std::string &line : logLines())
        if (line.find(text) != std::string::npos)
            n++;
    return n;
}

/* Start the driver on a repeater channel, and stop it after the scenario. */
struct Repeater {
    Repeater(uint8_t timeslot = 2, uint8_t cc = CC)
    {
        reset();
        startRepeater(cc, timeslot);
    }
    ~Repeater()
    {
        stop();
        trace();
    }
};

/* Downlink slots of idle bursts, the TC bit alternating from tc. */
static uint8_t idleSlots(int count, uint8_t tc, uint8_t cc = CC)
{
    for (int i = 0; i < count; i++) {
        downlinkSlot(IDLE_BURST, cc, tc);
        tc ^= 1;
    }
    return tc;
}

/* Align on timeslot 2: eight slots, the TC bits ending on 0. */
static uint8_t align()
{
    return idleSlots(8, 1);
}

/* Align, then start a call with its voice header. */
static uint8_t alignAndCall()
{
    uint8_t tc = align();
    setLinkControl(groupLc(314431, 3101003));
    burst(HEADER, CC);
    return tc;
}

TEST_CASE("DMR repeater channel sets the chip up for its timeslot",
          "[dmr][harness][repeater]")
{
    for (uint8_t ts : { 1, 2 }) {
        Repeater r(ts);
        std::vector<Write> ws = writes();

        // The repeater mode of the timeslot, written before the sync pattern
        size_t mode = ws.size(), sync = ws.size();
        for (size_t i = 0; i < ws.size(); i++) {
            if ((ws[i].page == CONFIG) && (ws[i].addr == 0x10)
                && (mode == ws.size()))
                mode = i;
            if ((ws[i].page == AUX) && (ws[i].addr == 0x04)
                && (sync == ws.size()))
                sync = i;
        }
        REQUIRE(lastWrite(CONFIG, 0x10) == ((ts == 2) ? 0x6D : 0x6C));
        REQUIRE(mode < sync);
        REQUIRE(lastWrite(CONFIG, 0x5F) == 0xC0);
    }
}

TEST_CASE("DMR repeater aligns on the TC bits, then receives every other slot",
          "[dmr][harness][repeater]")
{
    Repeater r;
    clearWrites();

    // Searching: the slots are left as they are
    uint8_t tc = idleSlots(7, 1);
    REQUIRE(writes(CONFIG, 0x41).empty());
    REQUIRE(logged("repeater: aligned") == 0);

    // The eighth slot aligns: the other timeslot comes next, then ours
    tc = idleSlots(1, tc);
    REQUIRE(logged("repeater: aligned on timeslot 2") == 1);
    REQUIRE(slotActions() == std::vector<uint8_t>({ 0x00 }));

    idleSlots(4, tc);
    REQUIRE(slotActions()
            == std::vector<uint8_t>({ 0x00, 0x40, 0x00, 0x40, 0x00 }));
}

TEST_CASE("DMR repeater takes no burst until aligned",
          "[dmr][harness][repeater]")
{
    Repeater r;
    idleSlots(5, 1);
    clearWrites();

    setLinkControl(groupLc(314431, 3101003));
    burst(HEADER, CC);
    REQUIRE_FALSE(poll().valid);
    REQUIRE_FALSE(writes(CONFIG, 0x83).empty()); // Acknowledged all the same
}

TEST_CASE("DMR repeater call is received on its timeslot only",
          "[dmr][harness][repeater]")
{
    Repeater r;
    uint8_t tc = alignAndCall();
    RxState rx = poll();
    REQUIRE(rx.valid);
    REQUIRE(rx.dstId == 314431);
    clearWrites();

    // During the call: receive our slot, silence the other
    for (int n = 0; n < 4; n++) {
        downlinkSlot(FRAME_B, CC, tc);
        tc ^= 1;
    }
    std::vector<uint8_t> actions = slotActions();
    REQUIRE(actions.size() == 4);
    for (size_t i = 0; i < actions.size(); i++)
        REQUIRE(actions[i] == ((i % 2 == 0) ? 0x50 : 0x00));
}

TEST_CASE("DMR repeater keeps the alignment when a call ends",
          "[dmr][harness][repeater]")
{
    Repeater r;
    uint8_t tc = alignAndCall();
    REQUIRE(poll().valid);
    clearWrites();

    // The terminator ends the call without restarting the receiver
    burst(TERMINATOR, CC);
    REQUIRE_FALSE(poll().valid);
    REQUIRE_FALSE(rearmedIn(writes()));

    // The repeater's hang time follows: still aligned, still alternating
    idleSlots(4, tc);
    REQUIRE_FALSE(rearmedIn(writes()));
    REQUIRE(logged("downlink lost") == 0);
    std::vector<uint8_t> actions = slotActions();
    REQUIRE(actions.size() == 4);
    REQUIRE(actions[0] != actions[1]);

    // The next call is taken straight away, no new alignment needed
    setLinkControl(groupLc(314431, 3217219));
    burst(HEADER, CC);
    REQUIRE(poll().valid);
    REQUIRE(poll().srcId == 3217219);
    REQUIRE(logged("repeater: aligned") == 1);
}

TEST_CASE("DMR repeater reads voice from its timeslot's block",
          "[dmr][harness][repeater]")
{
    Repeater r;
    alignAndCall();

    Bytes ours = voicePayload();
    Bytes other(27, 0x11);
    setRepeaterPayload(1, ours, false);
    setRepeaterPayload(0, other, false);

    burst(FRAME_B, CC);
    REQUIRE(audioFramesPushed() == 3);
    REQUIRE(lastFramePushed() == Bytes(ours.begin() + 18, ours.end()));
}

TEST_CASE("DMR repeater follows the voice sync into the other block",
          "[dmr][harness][repeater]")
{
    Repeater r;
    alignAndCall();

    // Frame A's sync shows up in block 0: our timeslot's payload is there
    Bytes moved = voicePayload();
    moved[18] = 0x42;
    setRepeaterPayload(0, moved, true);
    setRepeaterPayload(1, Bytes(27, 0x11), false);
    burst(FRAME_A, CC);
    REQUIRE(lastFramePushed() == Bytes(moved.begin() + 18, moved.end()));

    // And it stays there for the rest of the superframe
    Bytes next = voicePayload();
    next[18] = 0x43;
    setRepeaterPayload(0, next, false);
    burst(FRAME_B, CC);
    REQUIRE(lastFramePushed() == Bytes(next.begin() + 18, next.end()));
}

TEST_CASE("DMR repeater downlink is lost on a radio's sync",
          "[dmr][harness][repeater]")
{
    Repeater r;
    uint8_t tc = align();
    clearWrites();

    downlinkSlot(IDLE_BURST, CC, tc, false);
    REQUIRE(rearmedIn(writes()));
    REQUIRE(logged("repeater: downlink lost") == 1);

    // The search starts over
    clearWrites();
    idleSlots(7, 1);
    REQUIRE(writes(CONFIG, 0x41).empty());
}

TEST_CASE("DMR repeater downlink is lost after four bad slots",
          "[dmr][harness][repeater]")
{
    Repeater r;
    uint8_t tc = align();
    clearWrites();

    idleSlots(3, tc, 5);
    REQUIRE_FALSE(rearmedIn(writes()));

    idleSlots(1, tc ^ 1, 5);
    REQUIRE(rearmedIn(writes()));
    REQUIRE(logged("repeater: downlink lost") == 1);
}

TEST_CASE("DMR repeater call is lost after 25 slots without a voice sync",
          "[dmr][harness][repeater]")
{
    Repeater r;
    uint8_t tc = alignAndCall();
    REQUIRE(poll().valid);
    clearWrites();

    for (int n = 0; n < 24; n++) {
        downlinkSlot(FRAME_B, CC, tc);
        tc ^= 1;
    }
    REQUIRE(poll().valid);

    downlinkSlot(FRAME_B, CC, tc); // The 25th
    REQUIRE_FALSE(poll().valid);
    REQUIRE(rearmedIn(writes()));
    REQUIRE(logged("downlink lost, no voice sync") == 1);
}

TEST_CASE("DMR repeater keeps its timeslot over a missed slot interrupt",
          "[dmr][harness][repeater]")
{
    Repeater r;
    uint8_t tc = align();
    idleSlots(1, tc); // Our slot comes next
    clearWrites();

    // Two boundaries handled at once: our slot comes next again
    setReg(CONFIG, 0x51, IDLE_BURST);
    setReg(CONFIG, 0x52, CC << 4);
    setReg(CONFIG, 0x5F, 0xC1);
    timeslot(2);
    REQUIRE(slotActions() == std::vector<uint8_t>({ 0x40 }));
}

TEST_CASE("DMR channel change between direct and repeater sets the chip up",
          "[dmr][harness][repeater]")
{
    Driver d;
    clearWrites();

    configure(true, 2, CC);
    REQUIRE(lastWrite(CONFIG, 0x10) == 0x6D);
    REQUIRE(logged("dmr: repeater channel, timeslot 2") == 1);

    clearWrites();
    configure(false, 1, CC);
    REQUIRE(lastWrite(CONFIG, 0x10) == 0x6A);
}

TEST_CASE("DMR repeater call that goes silent restarts the search",
          "[dmr][harness][repeater]")
{
    Repeater r;
    alignAndCall();
    REQUIRE(poll().valid);
    clearWrites();

    // The downlink stops altogether: no more slot interrupts
    advance(600);
    poll();
    waitIdle();
    REQUIRE_FALSE(poll().valid);
    REQUIRE(rearmedIn(writes()));
    REQUIRE(logged("downlink lost, timeout") == 1);
}

TEST_CASE("DMR repeater that falls silent is searched for again",
          "[dmr][harness][repeater]")
{
    Repeater r;
    align();
    clearWrites();

    // Slot interrupts stop with the downlink: the alignment is stale
    advance(200);
    poll();
    waitIdle();
    REQUIRE(rearmedIn(writes()));
    REQUIRE(logged("repeater: downlink lost, silent") == 1);

    // Polled again, it does not restart twice
    advance(200);
    poll();
    waitIdle();
    REQUIRE(logged("repeater: downlink lost") == 1);
}

/* What the driver wrote at slot boundaries while transmitting. */
struct TxSlot {
    std::vector<uint8_t> timing;  // 0x40
    std::vector<uint8_t> actions; // 0x41
    std::vector<uint8_t> bursts;  // 0x50
};

/* Raise the timeslot interrupt count times at once, as the driver sees it. */
static TxSlot txSlot(int count = 1)
{
    clearWrites();
    timeslot(count);

    TxSlot t;
    for (const Write &w : writes()) {
        if (w.page != CONFIG)
            continue;
        if (w.addr == 0x40)
            t.timing.push_back(w.value);
        if (w.addr == 0x41)
            t.actions.push_back(w.value);
        if (w.addr == 0x50)
            t.bursts.push_back(w.value);
    }
    return t;
}

/* Run slots, gathering what was written over all of them. */
static TxSlot txSlots(int count)
{
    TxSlot all;
    for (int n = 0; n < count; n++) {
        TxSlot t = txSlot();
        all.timing.insert(all.timing.end(), t.timing.begin(), t.timing.end());
        all.actions.insert(all.actions.end(), t.actions.begin(),
                           t.actions.end());
        all.bursts.insert(all.bursts.end(), t.bursts.begin(), t.bursts.end());
    }
    return all;
}

TEST_CASE("DMR repeater channel is ready to transmit only while aligned",
          "[dmr][harness][repeater][tx]")
{
    Repeater r;
    REQUIRE(txReadiness() == DMR_TX_WAKEUP);

    uint8_t tc = idleSlots(7, 1);
    REQUIRE(txReadiness() == DMR_TX_WAKEUP);
    tc = idleSlots(1, tc);
    REQUIRE(txReadiness() == DMR_TX_READY);

    downlinkSlot(IDLE_BURST, CC, tc, false);
    REQUIRE(txReadiness() == DMR_TX_WAKEUP);

    // Direct mode is always ready
    configure(false, 1, CC);
    REQUIRE(txReadiness() == DMR_TX_READY);
}

TEST_CASE("DMR repeater key-up without the downlink's timing sends nothing",
          "[dmr][harness][repeater][tx]")
{
    Repeater r;
    idleSlots(5, 1);
    clearWrites();

    keyUp(true, 314431, 3101003);
    REQUIRE_FALSE(txActive());
    REQUIRE(logged("tx: refused, repeater timing lost") == 1);
    REQUIRE_FALSE(wroteAny(writes(), { 0x40, 0x41, 0x50 }));
}

TEST_CASE("DMR repeater transmission goes out on its timeslot, on the "
          "downlink's timing",
          "[dmr][harness][repeater][tx]")
{
    Repeater r;
    align(); // The other timeslot comes next
    clearWrites();

    // Key-up leaves the timing and the slot actions as they are
    keyUp(true, 314431, 3101003);
    REQUIRE(txActive());
    REQUIRE(writes(CONFIG, 0x40).empty());
    REQUIRE(writes(CONFIG, 0x41).empty());

    // The first slot takes the downlink's timing, then sends on our timeslot
    clearWrites();
    timeslot();
    REQUIRE(configWrites(writes(), { 0x40, 0x41 })
            == std::vector<RegWrite>({ { 0x40, 0xC3 }, { 0x41, 0x80 } }));

    // Three headers in all, on every other slot, the timing left alone
    TxSlot t = txSlots(5);
    REQUIRE(t.timing.empty());
    REQUIRE(t.actions
            == std::vector<uint8_t>({ 0x00, 0x80, 0x00, 0x80, 0x00 }));
    REQUIRE(t.bursts == std::vector<uint8_t>({ 0x10, 0x10 }));

    // Voice A..F
    t = txSlots(12);
    REQUIRE(t.bursts
            == std::vector<uint8_t>({ 0x08, 0x18, 0x28, 0x38, 0x48, 0x58 }));
    REQUIRE(payloadsWritten().size() == 6);

    // Unkeyed after frame B: voice to frame F, the terminator, the other
    // slot, the end
    REQUIRE(txSlots(4).bursts == std::vector<uint8_t>({ 0x08, 0x18 }));
    unkey();
    std::vector<uint8_t> bursts;
    uint8_t lastAction = 0x00;
    for (int n = 0; txActive() && (n < 40); n++) {
        t = txSlot();
        REQUIRE(t.timing.empty());
        REQUIRE_FALSE(((t.actions.back() == 0x80) && (lastAction == 0x80)));
        lastAction = t.actions.back();
        bursts.insert(bursts.end(), t.bursts.begin(), t.bursts.end());
    }
    REQUIRE_FALSE(txActive());
    REQUIRE(bursts == std::vector<uint8_t>({ 0x28, 0x38, 0x48, 0x58, 0x20 }));
    REQUIRE(lastAction == 0x40);

    // Back to receive still aligned: the vocoder reset, no re-arm, the timing
    // left on the downlink's and the two timeslots alternating on
    clearWrites();
    idleSlots(5, 0);
    REQUIRE(containsSequence(
        configWrites(writes(), { 0x20, 0x21, 0x22 }),
        { { 0x20, 0x00 }, { 0x21, 0x02 }, { 0x22, 0x16 }, { 0x22, 0x46 } }));
    REQUIRE_FALSE(rearmedIn(writes()));
    REQUIRE(writes(CONFIG, 0x40).empty());
    REQUIRE(slotActions()
            == std::vector<uint8_t>({ 0x00, 0x40, 0x00, 0x40, 0x00 }));
    REQUIRE(logged("downlink lost") == 0);
    REQUIRE(txReadiness() == DMR_TX_READY);
}

TEST_CASE("DMR repeater transmission keeps its timeslot over a missed slot "
          "interrupt",
          "[dmr][harness][repeater][tx]")
{
    Repeater r;
    align();
    keyUp(true, 314431, 3101003);
    REQUIRE(txSlot().actions == std::vector<uint8_t>({ 0x80 }));

    // Two boundaries handled at once: our slot comes next again
    REQUIRE(txSlot(2).actions == std::vector<uint8_t>({ 0x80 }));
    REQUIRE(txSlot().actions == std::vector<uint8_t>({ 0x00 }));
}

TEST_CASE("DMR repeater key-up in the hang time after a call",
          "[dmr][harness][repeater][tx]")
{
    Repeater r;
    uint8_t tc = alignAndCall();
    REQUIRE(poll().valid);
    burst(TERMINATOR, CC);
    REQUIRE_FALSE(poll().valid);
    idleSlots(2, tc); // Ours, then the other: ours comes next

    keyUp(true, 314431, 3101003);
    TxSlot t = txSlot();
    REQUIRE(t.timing == std::vector<uint8_t>({ 0xC3 }));
    REQUIRE(t.actions == std::vector<uint8_t>({ 0x80 }));
    REQUIRE(t.bursts == std::vector<uint8_t>({ 0x10 }));
}

/* The bytes written to the data page, in order. */
static Bytes dataWritten(const std::vector<Write> &ws)
{
    Bytes out;
    for (const Write &w : ws)
        if (w.page == DATA)
            out.push_back(w.value);
    return out;
}

TEST_CASE("DMR repeater wake-up sends its control block on the chip's own "
          "timing",
          "[dmr][harness][repeater][tx]")
{
    Repeater r;
    idleSlots(3, 1); // Part way through a search
    clearWrites();

    // The search restarts, then transmission on the chip's own timing
    startWakeup(3101003);
    REQUIRE(txActive());
    std::vector<RegWrite> start = configWrites(writes(), { 0x40, 0x41 });
    REQUIRE(containsSequence(start, REARM));
    REQUIRE(start.size() >= 2);
    REQUIRE(start[start.size() - 2] == RegWrite(0x41, 0x00));
    REQUIRE(start.back() == RegWrite(0x40, 0xA3));

    // The first slot carries the wake-up block
    clearWrites();
    timeslot();
    REQUIRE(slotActions() == std::vector<uint8_t>({ 0x80 }));
    REQUIRE(dataWritten(writes())
            == Bytes({ 0xB8, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x2F, 0x51,
                       0x4B }));
    REQUIRE(writes(CONFIG, 0x50).back().value == 0x30);
    std::vector<RegWrite> order = configWrites(writes(), { 0x41, 0x50 });
    REQUIRE(order.front() == RegWrite(0x41, 0x80));

    // The next is left empty, and the one after ends it
    clearWrites();
    timeslot();
    REQUIRE(slotActions() == std::vector<uint8_t>({ 0x00 }));
    REQUIRE(txActive());
    timeslot();
    REQUIRE_FALSE(txActive());
    REQUIRE(logged("tx: wake-up sent") == 1);

    // The receiver comes back searching for the downlink
    clearWrites();
    timeslot();
    REQUIRE(rearmedIn(writes()));
    REQUIRE(txReadiness() == DMR_TX_WAKEUP);

    // The repeater answers: aligned, ready to key up
    idleSlots(8, 1);
    REQUIRE(txReadiness() == DMR_TX_READY);
}

TEST_CASE("DMR repeater wake-up ends on its own if the slot clock stops",
          "[dmr][harness][repeater][tx]")
{
    Repeater r;
    startWakeup(3101003);
    advance(1600);
    txTask();
    waitIdle();
    REQUIRE_FALSE(txActive());

    // A key-up after it is not taken for a wake-up
    clearWrites();
    timeslot(); // The receiver comes back
    idleSlots(8, 1);
    keyUp(true, 314431, 3101003);
    timeslot();
    REQUIRE(writes(CONFIG, 0x50).back().value == 0x10);
}

TEST_CASE("DMR wake-up on a direct channel sends nothing", "[dmr][harness][tx]")
{
    Driver d;
    clearWrites();
    startWakeup(3101003);
    REQUIRE_FALSE(txActive());
    REQUIRE(writes().empty());
}
