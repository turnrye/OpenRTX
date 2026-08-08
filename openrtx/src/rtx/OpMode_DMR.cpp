/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "interfaces/delays.h"
#include "drivers/baseband/dmr.h"
#include "drivers/baseband/dmr_audio.h"
#include "core/dmr_debug.h"
#include "rtx/OpMode_DMR.hpp"

/**
 * Decide whether a call is meant for this radio. A group call has to be to
 * the talkgroup the radio is set to and a private call to its own DMR ID,
 * which is how a radio hears somebody calling it directly whatever talkgroup
 * it sits on. The promiscuous setting takes everything the channel carries
 * instead, which is what a monitoring receiver wants and what the radio does
 * by default while DMR is being brought up.
 *
 * Policy reads the rtx status here, in the operating mode, exactly as the
 * M17 mode evaluates its CAN and callsign checks: the driver carries no
 * opinion on whose call this is.
 *
 * @param status: current rtx status, carrying the radio's DMR settings.
 * @param groupCall: true for a group call, false for a private one.
 * @param dstId: destination the call is addressed to.
 * @return true if the call is to be received.
 */
static bool callAccepted(const rtxStatus_t *status, const bool groupCall,
                         const uint32_t dstId)
{
    if(status->dmrRxPromisc != 0)
        return true;

    if(groupCall)
        return dstId == status->dmrTalkgroup;

    return (status->dmrId != 0) && (dstId == status->dmrId);
}

void OpMode_DMR::enable()
{
    /*
     * The baseband is brought up on the first update step, as the color code
     * it has to be configured with is only known once the rtx status is at
     * hand.
     */
    enabled = false;
    startRx = false;
    startTx = false;
    callActive = false;

    for(int slot = 0; slot < DMR_SLOT_COUNT; slot++)
        dmr_callState_init(&rxCall[slot]);
}

void OpMode_DMR::disable()
{
    if(enabled)
    {
        dmr_terminate();
        enabled = false;
    }

    callActive = false;

    for(int slot = 0; slot < DMR_SLOT_COUNT; slot++)
        dmr_callState_init(&rxCall[slot]);

    platform_ledOff(GREEN);
    platform_ledOff(RED);
    radio_disableRtx();
}

void OpMode_DMR::update(rtxStatus_t *const status, const bool newCfg)
{
    if((enabled == false) || newCfg)
    {
        struct dmrConfig cfg;
        cfg.dmrId         = status->dmrId;
        cfg.talkgroup     = status->dmrTalkgroup;
        cfg.colorCode     = status->dmrColorCode;
        cfg.rxPromiscuous = (status->dmrRxPromisc != 0);

        uint8_t colorCode = status->dmrColorCode & 0x0F;

        if(enabled == false)
        {
            dmr_init(&cfg);
            enabled = true;
            startRx = true;
        }
        else
        {
            dmr_setConfig(&cfg);

            /*
             * A changed colour code restarts the modem's protocol modules,
             * which drops every call in progress at the chip: the link
             * control tracking starts over with it, so the next
             * transmission is confirmed afresh rather than trusted on the
             * strength of a call the modem no longer has.
             */
            if(colorCode != lastColorCode)
                dmr_callState_resetLcTracking(&rxCall[0]);
        }

        lastColorCode = colorCode;
    }

    switch(status->opStatus)
    {
        case OFF:
            offState(status);
            break;

        case RX:
            rxState(status);
            break;

        case TX:
            txState(status);
            break;

        default:
            break;
    }
}

void OpMode_DMR::offState(rtxStatus_t *const status)
{
    if(startRx)
    {
        status->opStatus = RX;
        return;
    }

    if(platform_getPttStatus() && (status->txDisable == 0))
    {
        startTx = true;
        status->opStatus = TX;
        return;
    }

    sleepFor(0u, 10u);
}

void OpMode_DMR::rxState(rtxStatus_t *const status)
{
    if(startRx)
    {
        radio_enableRx();
        platform_ledOff(RED);
        startRx = false;
    }

    struct dmrCallState *cs = &rxCall[0];

    /*
     * Poll the modem for a burst and run the call state on it. Everything
     * the state machine cannot decide is decided here - the accept policy,
     * the audio, the log lines - mirroring how the M17 mode owns its accept
     * checks and codec handling.
     */
    struct dmrBurst burst;
    if(dmr_rxPoll(&burst))
    {
        bool wasValid = cs->call.valid;

        uint8_t events = dmr_callState_feedBurst(cs, &burst, getTick());

        if(events & DMR_CALLSTATE_CC_OBSERVED)
        {
            dmrDebug_log(DMR_DBG_INFO, "call: colour code %u observed",
                         cs->call.colourCode);
        }

        /*
         * Play this burst's decodable frames once the gate is open. The
         * gate is judged on the call as it stood when the payload was read:
         * the same burst's link control may just have asked for a decision,
         * and that decision is taken below, after the audio of the call it
         * belongs to.
         */
        if(wasValid && (burst.voiceFrames != 0)
                    && dmr_callState_voiceGateOpen(cs) && dmrAudio_running())
        {
            for(int f = 0; f < 3; f++)
            {
                if(burst.voiceFrames & (1 << f))
                    dmrAudio_pushFrame(
                        &burst.payload[f * DMR_VOICE_FRAME_LENGTH]);
            }
        }

        /*
         * A newly confirmed call. Whether it is ours to receive is policy:
         * the call state knows how to tell a call, not whether we want it.
         */
        if(events & DMR_CALLSTATE_NEW_CALL)
        {
            bool     group = (cs->candidate.callType == DMR_CALL_GROUP);
            uint32_t dstId = cs->candidate.dstId;
            uint32_t srcId = cs->candidate.srcId;

            if(callAccepted(status, group, dstId) == false)
            {
                dmrDebug_log(DMR_DBG_INFO, "call: ignored, from %lu to %s %lu",
                             (unsigned long) srcId, group ? "TG" : "ID",
                             (unsigned long) dstId);

                /*
                 * A call in progress is dropped along with the rejected
                 * one: playback follows the call state, so leaving it up
                 * would let the rejected call be heard through the tail of
                 * the one it interrupted.
                 */
                if(cs->call.valid)
                    dmrAudio_stopRx();

                dmr_callState_reject(cs);
            }
            else
            {
                dmr_callState_accept(cs);

                dmrDebug_log(DMR_DBG_INFO, "call: from %lu to %s %lu",
                             (unsigned long) srcId, group ? "TG" : "ID",
                             (unsigned long) dstId);

                /* Start voice playback, if the vocoder is installed. */
                if(dmrAudio_startRx())
                    dmrDebug_log(DMR_DBG_INFO,
                                 "audio: voice playback started");
            }
        }
    }

    /*
     * The call state decides whether the call is over - behind a confirmed
     * terminator or by the carrier going away - and its decision is acted on
     * here, where the audio lives.
     */
    enum dmrCallEnd endReason = dmr_callState_update(cs, getTick());
    if(endReason != DMR_CALL_END_NONE)
    {
        dmrAudio_stopRx();
        dmrDebug_log(DMR_DBG_INFO, "call: ended (%s)",
                     (endReason == DMR_CALL_END_TERMINATOR) ? "terminator"
                                                            : "timeout");
    }

    /*
     * A call is surfaced only while its voice decodes. The chip refuses the
     * voice of a call on the wrong colour code but still hands over the link
     * control, so going by the link control alone put a silent ghost call on
     * screen for every rejected transmission; decodable voice is the one
     * signal that tracks what the chip actually accepted.
     */
    bool callShown = cs->call.valid && cs->call.voiceActive;

    status->dmrRxSrcId     = cs->call.srcId;
    status->dmrRxDstId     = cs->call.dstId;
    status->dmrRxGroupCall = (cs->call.callType == DMR_CALL_GROUP) ? 1 : 0;
    status->dmrLcValid     = (callShown) ? 1 : 0;

    /*
     * The observed colour code travels with its own validity bit: the chip
     * only delivers one for transmissions carrying a CACH, so a call from a
     * hotspot or repeater reports its code while one from a mobile station
     * transmitting direct never does, and the four bit status field cannot
     * say "unknown" on its own.
     */
    if(cs->call.colourCode <= 0x0F)
    {
        status->dmrRxColorCode = cs->call.colourCode;
        status->dmrRxCcValid   = 1;
    }
    else
    {
        status->dmrRxColorCode = 0;
        status->dmrRxCcValid   = 0;
    }

    /*
     * A call being received counts as an open squelch, which is what keeps the
     * user interface out of standby: without it the screen dims part way
     * through a call, exactly when it is being read.
     */
    callActive = callShown;

    if(callShown)
        platform_ledOn(GREEN);
    else
        platform_ledOff(GREEN);

    /*
     * Pressing the PTT hands over through the OFF state, as the M17 mode does,
     * so the transition into transmission goes through a single place.
     */
    if(platform_getPttStatus() && (status->txDisable == 0))
    {
        status->opStatus = OFF;
        return;
    }

    /*
     * Polled faster than the 30ms slot period so the receiver can be re-armed
     * within each slot: the chip only receives one slot per request and the
     * re-arming happens from this loop, not from the timeslot interrupt.
     */
    sleepFor(0u, 10u);
}

void OpMode_DMR::txState(rtxStatus_t *const status)
{
    if(startTx)
    {
        startTx = false;

        /*
         * Refuse to key up without a DMR ID: transmitting as ID zero would put
         * an unidentified call on the air. Say so on the tap, as an unexplained
         * dead PTT is otherwise a puzzle, and fall back to reception.
         */
        if(status->dmrId == 0)
        {
            dmrDebug_log(DMR_DBG_INFO,
                         "tx: refused, no DMR ID set (Settings > DMR > DMR ID)");
            startRx = true;
            status->opStatus = OFF;
            return;
        }

        callActive = false;
        platform_ledOff(GREEN);
        platform_ledOn(RED);

        radio_enableTx();
        dmr_startTx(true, status->dmrTalkgroup, status->dmrId);
    }

    /*
     * Periodic transmit housekeeping: keeps the interrupt watchdog fed while
     * the receive poll, which normally feeds it, is not running.
     */
    dmr_txTask();

    if(platform_getPttStatus() == false)
        dmr_stopTx();

    /*
     * The state machine sends the terminator and returns the chip to reception
     * on its own; wait for it to finish before handing back so the terminator
     * is not cut off.
     */
    if(dmr_txActive() == false)
    {
        platform_ledOff(RED);

        /*
         * Shut the radio down on the way back to reception. Going straight from
         * transmitting to receiving leaves the transmitter chip in its transmit
         * function mode and the receiver never returns until the operating mode
         * is cycled by hand. The FM and M17 modes get this from shutting down on
         * every transition, but doing that here would also shut the radio down
         * immediately before keying up, which leaves the synthesiser unsettled
         * for the first bursts and costs the far end the call header.
         */
        radio_disableRtx();

        /*
         * Our own transmission took the receive path away; the call tracking
         * starts over with it, its liveness clock restarted so an old call
         * does not time out on the spot.
         */
        dmr_callState_restartTracking(&rxCall[0], getTick());

        startRx = true;
        status->opStatus = OFF;
        return;
    }

    sleepFor(0u, 10u);
}
