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
 * The emulator has no baseband to talk to, so the receiver never reports a
 * call. This exists to let the mode be selected and its user interface be
 * exercised on the host.
 */

static struct dmrConfig dmrCfg;

void dmr_init(const struct dmrConfig *cfg)
{
    dmrCfg = *cfg;
    dmrDebug_log(DMR_DBG_INFO, "dmr: emulated receiver, color code %u",
                 dmrCfg.colorCode & 0x0F);
}

void dmr_terminate()
{
    dmrDebug_log(DMR_DBG_INFO, "dmr: receiver stopped");
}

void dmr_setConfig(const struct dmrConfig *cfg)
{
    dmrCfg = *cfg;
}

void dmr_task(struct dmrRxState *rx)
{
    memset(rx, 0, sizeof(struct dmrRxState));
    rx->colorCode = DMR_CC_UNKNOWN;
}

void dmr_startTx(const bool groupCall, const uint32_t dstId,
                 const uint32_t srcId)
{
    (void) groupCall;
    (void) dstId;
    (void) srcId;

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

void dmr_txTimeslotIsr()
{
}

#endif /* CONFIG_DMR */
