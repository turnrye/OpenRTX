/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "interfaces/dmr.h"
#include "core/dmr_audio.h"
#include "core/dmr_debug.h"
#include "core/tones.h"
#include "rtx/OpMode_DMR.hpp"

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
    txRefused = false;
    waking = false;
    wakeupSending = false;
    micPending = false;
}

void OpMode_DMR::disable()
{
    if (enabled) {
        dmr_terminate();
        enabled = false;
    }

    /*
     * The baseband is stopped first, so that it hands no more voice frames
     * over while the audio stops.
     */
    dmrAudio_stopRx();
    dmrAudio_stopTx();
    tones_stop();
    micPending = false;

    platform_ledOff(GREEN);
    platform_ledOff(RED);
    radio_disableRtx();
}

/*
 * A channel whose transmit and receive frequencies are more than 10kHz apart
 * goes through a repeater, the rule the MD-UV380 stock firmware applies.
 */
static const freq_t REPEATER_MIN_SPLIT = 10000;

/**
 * Build the baseband configuration from the rtx status.
 *
 * @param status: rtx status the channel and radio settings come from.
 * @param config: destination configuration.
 */
static void buildConfig(const rtxStatus_t *status, struct dmrConfig *config)
{
    freq_t split = (status->txFrequency > status->rxFrequency) ?
                       (status->txFrequency - status->rxFrequency) :
                       (status->rxFrequency - status->txFrequency);

    config->access = (split > REPEATER_MIN_SPLIT) ? DMR_ACCESS_REPEATER :
                                                    DMR_ACCESS_DIRECT;
    config->colorCode = status->dmrColorCode;
    config->timeslot = status->dmrTimeslot + 1;
    config->ownId = status->dmrId;
    config->rxPromiscuous = status->dmrRxPromisc;
}

void OpMode_DMR::update(rtxStatus_t *const status, const bool newCfg)
{
    struct dmrConfig config;
    buildConfig(status, &config);

    if (enabled == false) {
        dmr_init(&config);
        enabled = true;
        startRx = true;
    } else if (newCfg) {
        dmr_configure(&config);
    }

    switch (status->opStatus) {
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
    /*
     * Switch the radio off between receiving and transmitting, as the FM and
     * M17 modes do: it takes the transceiver through its off state and clears
     * any tone or CTCSS the previous direction set up.
     */
    radio_disableRtx();

    if (startRx) {
        status->opStatus = RX;
        return;
    }

    if (pttAsksTx(status)) {
        startTx = true;
        status->opStatus = TX;
        return;
    }

    sleepFor(0u, 10u);
}

void OpMode_DMR::rxState(rtxStatus_t *const status)
{
    if (startRx) {
        radio_enableRx();
        platform_ledOff(RED);
        startRx = false;
    }

    struct dmrRxState rx;
    dmr_task(&rx);

    status->dmrRxSrcId = rx.srcId;
    status->dmrRxDstId = rx.dstId;
    status->dmrRxColorCode = rx.colorCode;
    status->dmrRxGroupCall = (rx.groupCall) ? 1 : 0;
    status->dmrLcValid = (rx.valid) ? 1 : 0;

    if (rx.valid)
        platform_ledOn(GREEN);
    else
        platform_ledOff(GREEN);

    /*
     * Play the voice of the call being received, unless it is encrypted, as in
     * OpenGD77. Playback is asked for on every step until it runs, as a voice
     * prompt may hold the speaker when the call comes in, or take it over
     * during the call, which ends playback until the speaker is free again.
     * Stopping also releases what a playback that ended on its own left.
     */
    bool play = rx.valid && (rx.encrypted == false);
    if (play && (dmrAudio_running() == false)) {
        if (dmrAudio_startRx())
            dmrDebug_log(DMR_DBG_INFO, "audio: voice playback started");
    } else if (play == false) {
        dmrAudio_stopRx();
    }

    /*
     * Pressing the PTT hands over through the OFF state, as the M17 mode does,
     * so the transition into transmission goes through a single place. The
     * call being received is no longer played.
     */
    if (pttAsksTx(status)) {
        dmrAudio_stopRx();
        status->opStatus = OFF;
        return;
    }

    /*
     * Reception runs on the baseband's own thread: this step only hands its
     * state over and lets it notice a call or a transmission winding down.
     */
    sleepFor(0u, 10u);
}

/*
 * A refused key-up is not tried again while the PTT stays down: it would be
 * refused on every step, and said so on the tap every time.
 */
bool OpMode_DMR::pttAsksTx(const rtxStatus_t *const status)
{
    if (platform_getPttStatus() == false) {
        txRefused = false;
        return false;
    }

    return (status->txDisable == 0) && (txRefused == false);
}

void OpMode_DMR::refuseTx(rtxStatus_t *const status, const char *reason)
{
    dmrDebug_log(DMR_DBG_INFO, "tx: refused, %s", reason);
    txRefused = true;
    startRx = true;
    status->opStatus = OFF;
}

/*
 * The Motorola talk permit tone, played as the call keys up as the sign to
 * start talking: G6, C6 and G6 of 35 ms, then E6 of 52 ms.
 */
static const char *const TALK_PERMIT_TONE = "permit:d=32,o=6,b=214:g,c,g,e.";

void OpMode_DMR::startMic()
{
    if (dmrAudio_startTx())
        dmrDebug_log(DMR_DBG_INFO, "audio: microphone encode started");
}

void OpMode_DMR::keyUp(rtxStatus_t *const status)
{
    platform_ledOff(GREEN);
    platform_ledOn(RED);

    /*
     * Capture and encode the microphone once the talk permit tone has played,
     * so that the tone does not go out with the voice; the baseband sends the
     * silence frame until then. Without the tone, the capture starts before
     * the key-up, so that the first voice frames are ready when the headers
     * are over.
     */
    micPending = tones_playRtttl(TALK_PERMIT_TONE);
    if (micPending == false)
        startMic();

    radio_enableTx();
    dmr_startTx(status->dmrPrivateCall == 0, status->dmrTalkgroup);
}

/*
 * Wake-ups sent before giving up, and the wait for the repeater after each, in
 * milliseconds: OpenGD77's, as the stock firmware takes them from the codeplug.
 */
static const uint8_t WAKEUP_ATTEMPTS = 4;
static const long long WAKEUP_INTERVAL = 600;

void OpMode_DMR::sendWakeup()
{
    wakeupCount += 1;
    wakeupTick = getTick();
    wakeupSending = true;

    dmrDebug_log(DMR_DBG_INFO, "tx: repeater wake-up %u of %u",
                 (unsigned)wakeupCount, (unsigned)WAKEUP_ATTEMPTS);

    platform_ledOn(RED);
    radio_disableRtx();
    radio_enableTx();
    dmr_startWakeup();
}

void OpMode_DMR::wakeupState(rtxStatus_t *const status)
{
    /* Keeps the interrupt watchdog fed, as dmr_task() is not called. */
    dmr_txTask();

    /* The wake-up goes out in full before the radio goes back to receive. */
    if (wakeupSending) {
        if (dmr_txActive()) {
            sleepFor(0u, 10u);
            return;
        }

        wakeupSending = false;
        platform_ledOff(RED);
        radio_disableRtx();
        radio_enableRx();
    }

    if (platform_getPttStatus() == false) {
        waking = false;
        startRx = true;
        status->opStatus = OFF;
        return;
    }

    if (dmr_txReadiness() == DMR_TX_READY) {
        waking = false;
        dmrDebug_log(DMR_DBG_INFO, "tx: the repeater answered");
        radio_disableRtx();
        keyUp(status);
        return;
    }

    if ((getTick() - wakeupTick) >= WAKEUP_INTERVAL) {
        if (wakeupCount >= WAKEUP_ATTEMPTS) {
            waking = false;
            refuseTx(status, "the repeater does not answer");
            return;
        }

        sendWakeup();
    }

    sleepFor(0u, 10u);
}

void OpMode_DMR::txState(rtxStatus_t *const status)
{
    if (startTx) {
        startTx = false;

        /*
         * Refuse to key up without a DMR ID: transmitting as ID zero would put
         * an unidentified call on the air. Say so on the tap, as an unexplained
         * dead PTT is otherwise a puzzle, and fall back to reception.
         */
        if (status->dmrId == 0) {
            refuseTx(status, "no DMR ID set (Settings > DMR > DMR ID)");
            return;
        }

        /*
         * Through a repeater the bursts follow its timing: one that is not
         * heard is woken up first.
         */
        switch (dmr_txReadiness()) {
            case DMR_TX_READY:
                keyUp(status);
                break;

            case DMR_TX_WAKEUP:
                waking = true;
                wakeupCount = 0;
                sendWakeup();
                return;

            default:
                refuseTx(status, "no repeater mode on this radio");
                return;
        }
    }

    if (waking) {
        wakeupState(status);
        return;
    }

    if (micPending && (tones_playing() == false)) {
        micPending = false;
        startMic();
    }

    /*
     * Periodic transmit housekeeping: keeps the interrupt watchdog fed while
     * dmr_task(), which normally feeds it, is not called.
     */
    dmr_txTask();

    if (platform_getPttStatus() == false)
        dmr_stopTx();

    /*
     * The baseband sends the terminator and returns the chip to reception on
     * its own; wait for it to finish before handing back so the terminator is
     * not cut off. It takes no more frames from then on, so the capture stops.
     */
    if (dmr_txActive() == false) {
        dmrAudio_stopTx();
        tones_stop();
        micPending = false;
        platform_ledOff(RED);
        startRx = true;
        status->opStatus = OFF;
        return;
    }

    sleepFor(0u, 10u);
}
