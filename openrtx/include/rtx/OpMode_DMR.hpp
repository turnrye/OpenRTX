/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPMODE_DMR_H
#define OPMODE_DMR_H

#include "rtx/OpMode.hpp"

/**
 * Operating mode for DMR.
 *
 * Receiving surfaces the link control of the decoded calls, which is what the
 * user interface shows as "last heard", and plays their voice. Transmitting
 * keys a call from the radio's own DMR ID carrying the microphone's voice. The
 * baseband runs the calls on a thread of its own; this mode hands their state
 * to the rtx status and starts and stops the audio: the playback while a call
 * is received, the microphone while transmitting.
 */
class OpMode_DMR : public OpMode
{
public:
    OpMode_DMR()
    {
    }

    ~OpMode_DMR()
    {
    }

    virtual void enable() override;

    virtual void disable() override;

    virtual void update(rtxStatus_t *const status, const bool newCfg) override;

    virtual opmode getID() override
    {
        return OPMODE_DMR;
    }

    virtual bool rxSquelchOpen() override
    {
        return false;
    }

private:
    /**
     * Idle handling: move on to reception once the baseband is up, or into
     * transmission when the PTT is pressed.
     */
    void offState(rtxStatus_t *const status);

    /**
     * Receive handling: run the receiver update, surface the decoded call and
     * play its voice, handing over to transmission when the PTT is pressed.
     */
    void rxState(rtxStatus_t *const status);

    /**
     * Transmit handling: key the call and the microphone, hold it while the
     * PTT is down, and let the terminator play out before returning to
     * reception on release.
     */
    void txState(rtxStatus_t *const status);

    /**
     * Wake-up handling, before keying up through a repeater that is not heard:
     * send the wake-up with the radio transmitting, listen for the repeater
     * with it receiving, and key up once the repeater answers, sending the
     * wake-up again a few times before giving up.
     */
    void wakeupState(rtxStatus_t *const status);

    /**
     * Send the repeater a wake-up, with the radio transmitting.
     */
    void sendWakeup();

    /**
     * Key the call and the microphone up.
     */
    void keyUp(rtxStatus_t *const status);

    /**
     * Refuse a key-up, saying why on the tap, and fall back to reception.
     *
     * @param reason: why, for the log.
     */
    void refuseTx(rtxStatus_t *const status, const char *reason);

    /**
     * Tell whether the PTT asks for a transmission: pressed, transmission
     * allowed, and not still held down from a key-up that was refused.
     */
    bool pttAsksTx(const rtxStatus_t *const status);

    bool enabled = false;       ///< Baseband brought up.
    bool startRx = false;       ///< Reception has still to be armed.
    bool startTx = false;       ///< A transmission has still to be keyed.
    bool txRefused = false;     ///< A key-up was refused, the PTT still held.
    bool waking = false;        ///< Waking the repeater up before keying up.
    bool wakeupSending = false; ///< The wake-up is being sent.
    uint8_t wakeupCount = 0;    ///< Wake-ups sent for this key-up.
    long long wakeupTick = 0;   ///< When the last wake-up was sent.
};

#endif /* OPMODE_DMR_H */
