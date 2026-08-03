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
 * user interface shows as "last heard". Transmitting keys a direct mode group
 * call carrying a constant silence payload: it exercises the whole outbound air
 * interface with no vocoder, the actual voice path is a later step.
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
        return false;
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
};

#endif /* OPMODE_DMR_H */
