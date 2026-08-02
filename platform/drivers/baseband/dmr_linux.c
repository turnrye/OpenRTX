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

#endif /* CONFIG_DMR */
