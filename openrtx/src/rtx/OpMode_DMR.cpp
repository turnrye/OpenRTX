/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "drivers/baseband/dmr.h"
#include "rtx/OpMode_DMR.hpp"

void OpMode_DMR::enable()
{
    /*
     * The receiver is brought up on the first update step, as the color code
     * it has to be configured with is only known once the rtx status is at
     * hand.
     */
    enabled = false;
}

void OpMode_DMR::disable()
{
    if(enabled)
    {
        dmr_terminate();
        enabled = false;
    }

    platform_ledOff(GREEN);
    radio_disableRtx();
}

void OpMode_DMR::update(rtxStatus_t *const status, const bool newCfg)
{
    if(enabled == false)
    {
        dmr_init(status->dmrColorCode);
        radio_enableRx();

        status->opStatus = RX;
        enabled          = true;
    }
    else if(newCfg)
    {
        dmr_setColorCode(status->dmrColorCode);
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

    sleepFor(0u, 30u);
}
