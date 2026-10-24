/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/DMR/LinkControl.h"

void dmr_buildVoiceLc(uint8_t *lc, const bool groupCall, const uint32_t dstId,
                      const uint32_t srcId)
{
    lc[0] = groupCall ? DMR_FLCO_GROUP : DMR_FLCO_PRIVATE;
    lc[1] = 0x00; // Feature set ID, standard
    lc[2] = 0x00; // Service options, none

    lc[3] = (dstId >> 16) & 0xFF;
    lc[4] = (dstId >> 8) & 0xFF;
    lc[5] = dstId & 0xFF;

    lc[6] = (srcId >> 16) & 0xFF;
    lc[7] = (srcId >> 8) & 0xFF;
    lc[8] = srcId & 0xFF;

    lc[9] = 0x00; // The chip fills in the error correction
    lc[10] = 0x00;
    lc[11] = 0x00;
}

void dmr_buildWakeupCsbk(uint8_t *csbk, const uint32_t srcId)
{
    csbk[0] = 0x80 | DMR_CSBKO_BS_DWN_ACT; // Last block
    csbk[1] = 0x00;                        // Feature set ID, standard
    csbk[2] = 0x00;                        // Reserved
    csbk[3] = 0x00;

    csbk[4] = 0xFF; // Repeater address, all
    csbk[5] = 0xFF;
    csbk[6] = 0xFF;

    csbk[7] = (srcId >> 16) & 0xFF;
    csbk[8] = (srcId >> 8) & 0xFF;
    csbk[9] = srcId & 0xFF;
}

/* Service options bit marking an encrypted call. */
#define DMR_SO_PRIVACY 0x40

bool dmr_parseVoiceLc(const uint8_t *lc, struct dmrVoiceCall *call)
{
    uint8_t flco = dmr_lcOpcode(lc);
    if ((flco != DMR_FLCO_GROUP) && (flco != DMR_FLCO_PRIVATE))
        return false;

    call->groupCall = (flco == DMR_FLCO_GROUP);
    call->encrypted = (lc[2] & DMR_SO_PRIVACY) != 0;
    call->dstId = (lc[3] << 16) | (lc[4] << 8) | lc[5];
    call->srcId = (lc[6] << 16) | (lc[7] << 8) | lc[8];

    return true;
}
