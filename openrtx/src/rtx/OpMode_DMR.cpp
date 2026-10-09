/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "interfaces/dmr.h"
#include "core/dmr_debug.h"
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
}

void OpMode_DMR::disable()
{
    if(enabled)
    {
        dmr_terminate();
        enabled = false;
    }

    platform_ledOff(GREEN);
    platform_ledOff(RED);
    radio_disableRtx();
}

void OpMode_DMR::update(rtxStatus_t *const status, const bool newCfg)
{
    if(enabled == false)
    {
        dmr_init(status->dmrColorCode);
        enabled = true;
        startRx = true;
    }
    else if(newCfg)
    {
        dmr_setColorCode(status->dmrColorCode);
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

    struct dmrRxState rx;
    dmr_task(&rx);

    status->dmrRxSrcId     = rx.srcId;
    status->dmrRxDstId     = rx.dstId;
    status->dmrRxColorCode = rx.colorCode;
    status->dmrRxGroupCall = (rx.groupCall) ? 1 : 0;
    status->dmrLcValid     = (rx.valid) ? 1 : 0;

    if(rx.valid)
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
        startRx = true;
        status->opStatus = OFF;
        return;
    }

    sleepFor(0u, 10u);
}
