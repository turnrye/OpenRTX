/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/DMR/LinkControl.h"
#include <string.h>

void dmr_buildVoiceLc(uint8_t *lc, const bool groupCall, const uint32_t dstId,
                      const uint32_t srcId)
{
    lc[0] = groupCall ? DMR_FLCO_GROUP : DMR_FLCO_PRIVATE;
    lc[1] = 0x00;                       // Feature set ID, standard
    lc[2] = 0x00;                       // Service options, none

    lc[3] = (dstId >> 16) & 0xFF;
    lc[4] = (dstId >>  8) & 0xFF;
    lc[5] =  dstId        & 0xFF;

    lc[6] = (srcId >> 16) & 0xFF;
    lc[7] = (srcId >>  8) & 0xFF;
    lc[8] =  srcId        & 0xFF;
}

void dmr_parseLc(struct dmrLinkControl *out, const uint8_t *lc)
{
    out->flco           = lc[0];
    out->featureSet     = lc[1];
    out->serviceOptions = lc[2];

    out->dstId = (lc[3] << 16) | (lc[4] << 8) | lc[5];
    out->srcId = (lc[6] << 16) | (lc[7] << 8) | lc[8];

    memcpy(out->payload, &lc[2], DMR_LC_LENGTH - 2);
}
