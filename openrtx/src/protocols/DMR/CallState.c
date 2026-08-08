/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/DMR/CallState.h"
#include "protocols/DMR/LinkControl.h"
#include <string.h>

/**
 * Take down the call being received. Everything the link control tracking
 * holds is dropped with it, so the next transmission is treated as a fresh
 * call from its first burst; the identities stay as "last heard".
 *
 * @param cs: state of the slot whose call ends.
 */
static void endCall(struct dmrCallState *cs)
{
    cs->call.valid = false;
    cs->call.voiceActive = false;
    cs->call.colourCode = DMR_CC_UNKNOWN;

    memset(cs->prevLc, 0, sizeof(cs->prevLc));
    cs->lcMatches = 0;
    cs->lcDecided = false;
    cs->voiceStreak = 0;
    cs->termPendingTick = 0;
}

void dmr_callState_init(struct dmrCallState *cs)
{
    memset(cs, 0, sizeof(struct dmrCallState));
    cs->call.colourCode = DMR_CC_UNKNOWN;
    cs->candidate.colourCode = DMR_CC_UNKNOWN;
}

uint8_t dmr_callState_feedBurst(struct dmrCallState *cs,
                                const struct dmrBurst *burst, const int64_t now)
{
    uint8_t flags = 0;

    /*
     * A burst event means a carrier is present, whatever the burst holds.
     * This keeps an established call alive across the voice bursts that sit
     * between the link control updates: those carry no fresh CRC-valid link
     * control, so gating the call on link control alone drops it every few
     * hundred milliseconds during a steady transmission.
     */
    cs->lastBurstTick = now;

    /*
     * Record an observed colour code on the call. The modem only reports a
     * code it trusts, and the protocol layer believes it as reported: which
     * observations are trustworthy is a property of the chip watching the
     * air, not of the air itself.
     */
    if ((burst->colourCode != DMR_CC_UNKNOWN)
        && (burst->colourCode != cs->call.colourCode)) {
        cs->call.colourCode = burst->colourCode;
    }

    /*
     * The voice gate. This drives two things: the empty slot of a direct
     * mode call decodes to noise and is dropped frame by frame, and a modem
     * can refuse the voice of a call while still delivering its link
     * control - so decodable voice arriving is what separates a call being
     * received from one being rejected.
     *
     * A single passing burst is not believed: noise clears the per-frame
     * check just often enough that a rejected call would blip through. Two
     * passing bursts within a short window are needed: real voice arrives
     * every 60ms so the second burst opens the gate, while the stray passes
     * land too far apart to ever pair up.
     *
     * A burst with nothing decodable leaves the gate alone. The receiver
     * hears both timeslots and the empty one's noise falls between the real
     * voice bursts, so closing the gate on it cut the audio into rhythmic
     * holes with a pop at each edge; the gate only closes once decodable
     * voice stops arriving altogether.
     */
    if (cs->call.valid && (burst->voiceFrames > 0)) {
        if ((now - cs->lastVoiceTick) > DMR_VOICE_CONFIRM_WINDOW)
            cs->voiceStreak = 0;

        if (cs->voiceStreak < 2)
            cs->voiceStreak += 1;

        cs->lastVoiceTick = now;
        cs->termPendingTick = 0;
    }

    /*
     * A terminator burst ends the call it belongs to, but it is not believed
     * on sight: a burst's type can be misread as a terminator in the middle
     * of a call, and acting on it immediately cut live calls into a stream
     * of end-and-restart flaps. What separates a real terminator from a
     * misread is what follows it - a transmission ends behind its
     * terminator, so the call is torn down only once the promised silence
     * holds, while a live call's next burst cancels the suspicion within a
     * burst time. Checking the terminator's link control instead was no
     * guard at all: it always names the call in progress.
     *
     * Any later burst which is not a terminator proves the transmission is
     * still running, whatever its payload holds. Cancelling only on
     * decodable voice was not enough: that depends on the payload being read
     * in time, and under load the events collapse, so a misread survived
     * long enough to cut a live call once or twice a keyup. An unclassified
     * burst says nothing either way and leaves the suspicion standing.
     */
    if (cs->call.valid && (burst->dataType != DMR_DATA_UNKNOWN)) {
        if (burst->dataType == DMR_DATA_TERMINATOR) {
            if (cs->termPendingTick == 0)
                cs->termPendingTick = now;
        } else {
            cs->termPendingTick = 0;
        }
    }

    if (burst->lcValid == false)
        return flags;

    /*
     * Only the voice call opcodes take part in confirming a call, and this
     * filter must run before the identity dedupe below: it is what lets a
     * station rotate talker alias or position blocks into its link control
     * without disturbing the confirmation of the call they decorate.
     */
    if ((burst->lc[0] != DMR_FLCO_GROUP) && (burst->lc[0] != DMR_FLCO_PRIVATE))
        return flags;

    /*
     * The identity dedupe: the same link control keeps coming back for as
     * long as the call lasts, so a change starts a fresh confirmation count.
     * The compare covers exactly the nine information octets - the parity
     * bytes never cross the modem boundary, so content differing between
     * burst types repeating one and the same call cannot masquerade as a
     * new call here.
     */
    if (memcmp(burst->lc, cs->prevLc, DMR_LC_LENGTH) != 0) {
        memcpy(cs->prevLc, burst->lc, DMR_LC_LENGTH);
        cs->lcMatches = 1;
        cs->lcDecided = false;
        return flags;
    }

    /*
     * Confirmed once the same link control has been seen enough times. The
     * counter is held at the threshold afterwards: it used to grow with
     * every burst and, being a byte, wrapped back through zero during a long
     * call, dropping the receive state for a burst or two each time round.
     */
    if (cs->lcMatches < DMR_LC_CONFIRM_COUNT) {
        cs->lcMatches += 1;
        if (cs->lcMatches < DMR_LC_CONFIRM_COUNT)
            return flags;
    }

    /*
     * Only the start of a call is acted on, not every burst of it: the
     * confirmed link control keeps repeating for as long as the call lasts,
     * and the decision it asks for only has to be taken once.
     */
    if (cs->lcDecided)
        return flags;

    cs->lcDecided = true;

    struct dmrLinkControl lc;
    dmr_parseLc(&lc, burst->lc);

    cs->candidate.srcId = lc.srcId;
    cs->candidate.dstId = lc.dstId;
    cs->candidate.callType = (lc.flco == DMR_FLCO_GROUP) ? DMR_CALL_GROUP :
                                                           DMR_CALL_PRIVATE;

    flags |= DMR_CALLSTATE_NEW_CALL;
    return flags;
}

void dmr_callState_accept(struct dmrCallState *cs)
{
    cs->call.srcId = cs->candidate.srcId;
    cs->call.dstId = cs->candidate.dstId;
    cs->call.callType = cs->candidate.callType;
    cs->call.valid = true;
}

void dmr_callState_reject(struct dmrCallState *cs)
{
    cs->call.valid = false;
}

bool dmr_callState_voiceGateOpen(const struct dmrCallState *cs)
{
    return cs->voiceStreak >= 2;
}

enum dmrCallEnd dmr_callState_update(struct dmrCallState *cs, const int64_t now)
{
    /*
     * Voice is considered flowing while decodable bursts keep arriving; the
     * window rides out the link control bursts interleaved with the voice,
     * which carry no frames of their own.
     */
    cs->call.voiceActive = cs->call.valid
                        && ((now - cs->lastVoiceTick) < DMR_RX_HOLD_TIMEOUT);

    if (cs->call.valid && (cs->termPendingTick != 0)
        && ((now - cs->termPendingTick) > DMR_TERMINATOR_CONFIRM)) {
        endCall(cs);
        return DMR_CALL_END_TERMINATOR;
    }

    /*
     * No burst is guaranteed to say when a call is over: a terminator may
     * never arrive, so the call is also dropped once no burst has been seen
     * for a while.
     */
    if (cs->call.valid && ((now - cs->lastBurstTick) > DMR_RX_HOLD_TIMEOUT)) {
        endCall(cs);
        return DMR_CALL_END_TIMEOUT;
    }

    return DMR_CALL_END_NONE;
}

void dmr_callState_restartTracking(struct dmrCallState *cs, const int64_t now)
{
    memset(cs->prevLc, 0, sizeof(cs->prevLc));
    cs->lcMatches = 0;
    cs->lcDecided = false;
    cs->voiceStreak = 0;
    cs->call.colourCode = DMR_CC_UNKNOWN;
    cs->call.voiceActive = false;
    cs->lastBurstTick = now;
}

void dmr_callState_resetLcTracking(struct dmrCallState *cs)
{
    memset(cs->prevLc, 0, sizeof(cs->prevLc));
    cs->lcMatches = 0;
    cs->lcDecided = false;
}
