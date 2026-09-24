/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/dmr_debug.h"
#include "hwconfig.h"
#include "dmr.h"
#include <string.h>

#if defined(CONFIG_DMR)

/*
 * DMR on the CS7000 Plus. The HR_C6000 DMR receive and transmit paths are not
 * brought up on this board yet: the mode can be selected and the vocoder
 * driver and its self-test run, but the receiver never reports a call and a
 * key-up is refused.
 */

static uint8_t configuredCc;

void dmr_init(const uint8_t colorCode)
{
    configuredCc = colorCode & 0x0F;
    dmrDebug_log(DMR_DBG_INFO, "dmr: CS7000P baseband not brought up yet, "
                 "color code %u", configuredCc);
}

void dmr_terminate()
{
    dmrDebug_log(DMR_DBG_INFO, "dmr: receiver stopped");
}

void dmr_setColorCode(const uint8_t colorCode)
{
    configuredCc = colorCode & 0x0F;
}

void dmr_task(struct dmrRxState *rx)
{
    memset(rx, 0, sizeof(struct dmrRxState));
}

void dmr_startTx(const bool groupCall, const uint32_t dstId,
                 const uint32_t srcId)
{
    (void) groupCall;
    (void) dstId;
    (void) srcId;

    dmrDebug_log(DMR_DBG_INFO, "dmr: CS7000P transmitter not brought up yet");
}

void dmr_stopTx()
{
}

void dmr_txTask()
{
}

bool dmr_txActive()
{
    return false;
}

void dmr_txTimeslotIsr()
{
}

#endif /* CONFIG_DMR */
