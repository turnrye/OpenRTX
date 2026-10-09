/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/dmr_debug.h"
#include "hwconfig.h"
#include "interfaces/dmr.h"
#include <string.h>

#if defined(CONFIG_DMR)

/*
 * The emulator has no baseband to talk to, so the receiver never reports a
 * call. This exists to let the mode be selected and its user interface be
 * exercised on the host.
 */

static uint8_t configuredCc;

void dmr_init(const uint8_t colorCode)
{
    configuredCc = colorCode & 0x0F;
    dmrDebug_log(DMR_DBG_INFO, "dmr: emulated receiver, color code %u",
                 configuredCc);
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
    (void)groupCall;
    (void)dstId;
    (void)srcId;

    dmrDebug_log(DMR_DBG_INFO, "dmr: emulated transmitter has no baseband");
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

#endif /* CONFIG_DMR */
