/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include "protocols/DMR/CallState.h"
#include "protocols/DMR/LinkControl.h"

/*
 * These tests pin down behaviours that were each bought with a bench
 * failure: the terminator handling is the third design after two that
 * flapped live calls, the voice gate closed an audio leak, and the confirm
 * counter once wrapped mid-call. A simplification that breaks one of them
 * will fail here instead of on the air.
 */

// A burst with every field at its "modem could not say" value.
static struct dmrBurst emptyBurst(void)
{
    struct dmrBurst b;
    memset(&b, 0, sizeof(b));
    b.dataType = DMR_DATA_UNKNOWN;
    b.syncClass = DMR_SYNC_UNKNOWN;
    b.colourCode = DMR_CC_UNKNOWN;
    return b;
}

// A CRC-good burst carrying a voice link control.
static struct dmrBurst lcBurst(uint32_t dst, uint32_t src, bool group = true)
{
    struct dmrBurst b = emptyBurst();
    b.dataType = DMR_DATA_VOICE_HEADER;
    b.crcOk = true;
    b.lcValid = true;
    dmr_buildVoiceLc(b.lc, group, dst, src);
    return b;
}

// A burst whose payload carried the given number of decodable voice frames.
static struct dmrBurst voiceBurst(uint8_t frames)
{
    struct dmrBurst b = emptyBurst();
    b.dataType = 0x04; // A voice sequence nibble, anything but 0x02
    b.voiceFrames = frames;
    return b;
}

// A burst whose sampled type reads as a terminator.
static struct dmrBurst terminatorBurst(void)
{
    struct dmrBurst b = emptyBurst();
    b.dataType = DMR_DATA_TERMINATOR;
    return b;
}

// Confirm and accept a group call to TG 9 from 3101001 at the given time.
static void establishCall(struct dmrCallState *cs, int64_t t)
{
    dmr_callState_init(cs);

    struct dmrBurst b = lcBurst(9, 3101001);
    REQUIRE(dmr_callState_feedBurst(cs, &b, t) == 0);
    REQUIRE(dmr_callState_feedBurst(cs, &b, t + 60) == DMR_CALLSTATE_NEW_CALL);

    dmr_callState_accept(cs);
    REQUIRE(cs->call.valid);
    REQUIRE(cs->call.srcId == 3101001);
    REQUIRE(cs->call.dstId == 9);
}

TEST_CASE("A link control confirms on its second sighting", "[dmr][callstate]")
{
    struct dmrCallState cs;
    dmr_callState_init(&cs);

    struct dmrBurst b = lcBurst(9, 3101001);
    REQUIRE(dmr_callState_feedBurst(&cs, &b, 0) == 0);
    REQUIRE(dmr_callState_feedBurst(&cs, &b, 60) == DMR_CALLSTATE_NEW_CALL);

    REQUIRE(cs.candidate.dstId == 9);
    REQUIRE(cs.candidate.srcId == 3101001);
    REQUIRE(cs.candidate.callType == DMR_CALL_GROUP);

    // Nothing is a call until the caller says so.
    REQUIRE(cs.call.valid == false);
}

TEST_CASE("A changed link control restarts the confirmation",
          "[dmr][callstate]")
{
    struct dmrCallState cs;
    dmr_callState_init(&cs);

    struct dmrBurst a = lcBurst(9, 3101001);
    struct dmrBurst b = lcBurst(91, 3101003);

    REQUIRE(dmr_callState_feedBurst(&cs, &a, 0) == 0);
    REQUIRE(dmr_callState_feedBurst(&cs, &b, 60) == 0);
    REQUIRE(dmr_callState_feedBurst(&cs, &b, 120) == DMR_CALLSTATE_NEW_CALL);

    REQUIRE(cs.candidate.dstId == 91);
    REQUIRE(cs.candidate.srcId == 3101003);
}

TEST_CASE("A private call names its type", "[dmr][callstate]")
{
    struct dmrCallState cs;
    dmr_callState_init(&cs);

    struct dmrBurst b = lcBurst(3101001, 3101003, false);
    dmr_callState_feedBurst(&cs, &b, 0);
    REQUIRE(dmr_callState_feedBurst(&cs, &b, 60) == DMR_CALLSTATE_NEW_CALL);

    REQUIRE(cs.candidate.callType == DMR_CALL_PRIVATE);
}

TEST_CASE("A confirmed call is decided once, however long it runs",
          "[dmr][callstate]")
{
    struct dmrCallState cs;
    establishCall(&cs, 0);

    // A long call: far more sightings than a byte-wide counter can hold.
    // The counter once wrapped through zero and dropped the call each time
    // round; no repeat sighting may ever announce the call again.
    struct dmrBurst b = lcBurst(9, 3101001);
    for (int i = 0; i < 300; i++) {
        REQUIRE(dmr_callState_feedBurst(&cs, &b, 120 + (i * 60)) == 0);
        REQUIRE(cs.call.valid);
    }
}

TEST_CASE("Unhandled opcodes run through the filter before the dedupe",
          "[dmr][callstate]")
{
    struct dmrCallState cs;
    dmr_callState_init(&cs);

    // A talker alias block between two sightings of the same voice link
    // control must not restart the confirmation: the filter runs before
    // the dedupe, so a station decorating its call with alias or position
    // blocks still confirms on the second voice sighting.
    struct dmrBurst voice = lcBurst(9, 3101001);
    struct dmrBurst alias = lcBurst(9, 3101001);
    alias.lc[0] = DMR_FLCO_TA_HEADER;

    REQUIRE(dmr_callState_feedBurst(&cs, &voice, 0) == 0);
    REQUIRE(dmr_callState_feedBurst(&cs, &alias, 60) == 0);
    REQUIRE(dmr_callState_feedBurst(&cs, &voice, 120)
            == DMR_CALLSTATE_NEW_CALL);
}

TEST_CASE("Unhandled opcodes do not disturb a call in progress",
          "[dmr][callstate]")
{
    struct dmrCallState cs;
    establishCall(&cs, 0);

    struct dmrBurst alias = lcBurst(9, 3101001);
    alias.lc[0] = DMR_FLCO_GPS_INFO;

    REQUIRE(dmr_callState_feedBurst(&cs, &alias, 120) == 0);

    // And the voice link control coming back is still the decided call.
    struct dmrBurst voice = lcBurst(9, 3101001);
    REQUIRE(dmr_callState_feedBurst(&cs, &voice, 180) == 0);
    REQUIRE(cs.call.valid);
}

TEST_CASE("A terminator ends the call only once its silence holds",
          "[dmr][callstate]")
{
    struct dmrCallState cs;
    establishCall(&cs, 0);

    struct dmrBurst term = terminatorBurst();
    dmr_callState_feedBurst(&cs, &term, 1000);

    // The silence has not held yet.
    REQUIRE(dmr_callState_update(&cs, 1100) == DMR_CALL_END_NONE);
    REQUIRE(cs.call.valid);

    // Now it has.
    REQUIRE(dmr_callState_update(&cs, 1160) == DMR_CALL_END_TERMINATOR);
    REQUIRE(cs.call.valid == false);
}

TEST_CASE("Any later classified burst cancels a misread terminator",
          "[dmr][callstate]")
{
    struct dmrCallState cs;
    establishCall(&cs, 0);

    struct dmrBurst term = terminatorBurst();
    dmr_callState_feedBurst(&cs, &term, 1000);

    // The next burst is not a terminator - and it does not even need to
    // pass its checks: a corrupted burst still proves the transmission is
    // running. Cancelling only on decodable voice was not enough, because
    // payload reads get missed under load.
    struct dmrBurst next = emptyBurst();
    next.dataType = 0x04;
    next.crcOk = false;
    dmr_callState_feedBurst(&cs, &next, 1060);

    REQUIRE(dmr_callState_update(&cs, 1200) == DMR_CALL_END_NONE);
    REQUIRE(cs.call.valid);
}

TEST_CASE("An unclassified burst neither arms nor cancels a terminator",
          "[dmr][callstate]")
{
    struct dmrCallState cs;
    establishCall(&cs, 0);

    // Arm, then feed an event whose registers went unsampled: it says
    // nothing either way, so the suspicion stands and the call ends.
    struct dmrBurst term = terminatorBurst();
    dmr_callState_feedBurst(&cs, &term, 1000);

    struct dmrBurst blind = emptyBurst();
    dmr_callState_feedBurst(&cs, &blind, 1060);

    REQUIRE(dmr_callState_update(&cs, 1160) == DMR_CALL_END_TERMINATOR);
}

TEST_CASE("An unclassified burst alone never ends a call", "[dmr][callstate]")
{
    struct dmrCallState cs;
    establishCall(&cs, 0);

    struct dmrBurst blind = emptyBurst();
    dmr_callState_feedBurst(&cs, &blind, 1000);

    REQUIRE(dmr_callState_update(&cs, 1200) == DMR_CALL_END_NONE);
    REQUIRE(cs.call.valid);
}

TEST_CASE("The voice gate opens on the second passing burst in the window",
          "[dmr][callstate]")
{
    struct dmrCallState cs;
    establishCall(&cs, 0);

    REQUIRE(dmr_callState_voiceGateOpen(&cs) == false);

    struct dmrBurst v = voiceBurst(3);
    dmr_callState_feedBurst(&cs, &v, 1000);
    REQUIRE(dmr_callState_voiceGateOpen(&cs) == false);

    dmr_callState_feedBurst(&cs, &v, 1060);
    REQUIRE(dmr_callState_voiceGateOpen(&cs));
}

TEST_CASE("Stray passes landing far apart never open the gate",
          "[dmr][callstate]")
{
    struct dmrCallState cs;
    establishCall(&cs, 0);

    // Noise clears the per-frame check now and then, but such passes land
    // much further apart than real voice ever does.
    struct dmrBurst v = voiceBurst(1);
    dmr_callState_feedBurst(&cs, &v, 1000);
    dmr_callState_feedBurst(&cs, &v, 1300);
    dmr_callState_feedBurst(&cs, &v, 1600);

    REQUIRE(dmr_callState_voiceGateOpen(&cs) == false);
}

TEST_CASE("A burst with nothing decodable leaves the gate alone",
          "[dmr][callstate]")
{
    struct dmrCallState cs;
    establishCall(&cs, 0);

    struct dmrBurst v = voiceBurst(3);
    dmr_callState_feedBurst(&cs, &v, 1000);
    dmr_callState_feedBurst(&cs, &v, 1060);
    REQUIRE(dmr_callState_voiceGateOpen(&cs));

    // The empty slot's noise falls between the real voice bursts; closing
    // the gate on it cut the audio into rhythmic holes.
    struct dmrBurst quiet = voiceBurst(0);
    dmr_callState_feedBurst(&cs, &quiet, 1090);
    REQUIRE(dmr_callState_voiceGateOpen(&cs));
}

TEST_CASE("A call with no bursts at all times out", "[dmr][callstate]")
{
    struct dmrCallState cs;
    establishCall(&cs, 0);

    REQUIRE(dmr_callState_update(&cs, 400) == DMR_CALL_END_NONE);
    REQUIRE(dmr_callState_update(&cs, 600) == DMR_CALL_END_TIMEOUT);
    REQUIRE(cs.call.valid == false);
}

TEST_CASE("Voice activity follows the decodable bursts", "[dmr][callstate]")
{
    struct dmrCallState cs;
    establishCall(&cs, 0);

    struct dmrBurst v = voiceBurst(3);
    dmr_callState_feedBurst(&cs, &v, 1000);

    dmr_callState_update(&cs, 1100);
    REQUIRE(cs.call.voiceActive);

    // Keep the carrier alive with voiceless bursts; the voice goes stale.
    struct dmrBurst quiet = voiceBurst(0);
    for (int64_t t = 1060; t <= 1600; t += 60)
        dmr_callState_feedBurst(&cs, &quiet, t);

    dmr_callState_update(&cs, 1600);
    REQUIRE(cs.call.valid);
    REQUIRE(cs.call.voiceActive == false);
}

TEST_CASE("The colour code is recorded as the modem reports it",
          "[dmr][callstate]")
{
    struct dmrCallState cs;
    establishCall(&cs, 0);

    REQUIRE(cs.call.colourCode == DMR_CC_UNKNOWN);

    struct dmrBurst b = voiceBurst(0);
    b.colourCode = 3;
    dmr_callState_feedBurst(&cs, &b, 1000);
    REQUIRE(cs.call.colourCode == 3);

    // A burst with no observation changes nothing.
    struct dmrBurst blind = emptyBurst();
    dmr_callState_feedBurst(&cs, &blind, 1060);
    REQUIRE(cs.call.colourCode == 3);

    // Colour code 0 is legal on air. Whether an observation of it is
    // trustworthy is the modem's judgement, taken before the burst gets
    // here; this layer believes what it is handed.
    struct dmrBurst zero = voiceBurst(0);
    zero.colourCode = 0;
    dmr_callState_feedBurst(&cs, &zero, 1120);
    REQUIRE(cs.call.colourCode == 0);
}

TEST_CASE("A rejected call drops the one in progress", "[dmr][callstate]")
{
    struct dmrCallState cs;
    establishCall(&cs, 0);

    // Another station confirms over the top; the caller turns it away.
    struct dmrBurst other = lcBurst(66, 3101003);
    dmr_callState_feedBurst(&cs, &other, 1000);
    REQUIRE(dmr_callState_feedBurst(&cs, &other, 1060)
            == DMR_CALLSTATE_NEW_CALL);

    dmr_callState_reject(&cs);
    REQUIRE(cs.call.valid == false);
}

TEST_CASE("An ended call keeps its identities as last heard",
          "[dmr][callstate]")
{
    struct dmrCallState cs;
    establishCall(&cs, 0);

    struct dmrBurst term = terminatorBurst();
    dmr_callState_feedBurst(&cs, &term, 1000);
    REQUIRE(dmr_callState_update(&cs, 1160) == DMR_CALL_END_TERMINATOR);

    REQUIRE(cs.call.srcId == 3101001);
    REQUIRE(cs.call.dstId == 9);

    // And the next transmission is a fresh call from its first burst.
    struct dmrBurst b = lcBurst(9, 3101001);
    REQUIRE(dmr_callState_feedBurst(&cs, &b, 2000) == 0);
    REQUIRE(dmr_callState_feedBurst(&cs, &b, 2060) == DMR_CALLSTATE_NEW_CALL);
}
