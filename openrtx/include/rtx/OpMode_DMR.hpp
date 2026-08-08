/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPMODE_DMR_H
#define OPMODE_DMR_H

#include "rtx/OpMode.hpp"
#include "protocols/DMR/CallState.h"

/**
 * Operating mode for DMR.
 *
 * The mode owns the call control: it polls the modem for bursts, runs the
 * per-slot call state of protocols/DMR on them, and takes the decisions the
 * state machine leaves to it - whether a confirmed call is accepted, when
 * the audio runs, what reaches the rtx status for the user interface. This
 * mirrors how the M17 mode owns its accept checks and codec handling; the
 * baseband driver below knows nothing about calls.
 */
class OpMode_DMR : public OpMode
{
public:

    OpMode_DMR() { }

    ~OpMode_DMR() { }

    virtual void enable() override;

    virtual void disable() override;

    virtual void update(rtxStatus_t *const status, const bool newCfg) override;

    virtual opmode getID() override
    {
        return OPMODE_DMR;
    }

    virtual bool rxSquelchOpen() override
    {
        return callActive;
    }

private:

    /**
     * Idle handling: move on to reception once the baseband is up, or into
     * transmission when the PTT is pressed.
     */
    void offState(rtxStatus_t *const status);

    /**
     * Receive handling: run the receiver update and surface the decoded call,
     * handing over to transmission when the PTT is pressed.
     */
    void rxState(rtxStatus_t *const status);

    /**
     * Transmit handling: key the call, hold it while the PTT is down, and let
     * the terminator play out before returning to reception on release.
     */
    void txState(rtxStatus_t *const status);

    bool enabled = false;   ///< Baseband brought up.
    bool startRx = false;   ///< Reception has still to be armed.
    bool startTx = false;   ///< A transmission has still to be keyed.
    bool callActive = false;    ///< A call is being received.
    uint8_t lastColorCode = 0;  ///< Colour code last pushed to the modem.

    /**
     * State of the call being received, one entry per logical slot. Direct
     * mode uses a single slot and nothing on air names one, so only the
     * first entry does anything today; the shape is the protocol's.
     */
    struct dmrCallState rxCall[DMR_SLOT_COUNT];
};

#endif /* OPMODE_DMR_H */
