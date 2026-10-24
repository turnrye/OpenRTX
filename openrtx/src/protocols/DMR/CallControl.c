/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/DMR/CallControl.h"
#include <string.h>

void dmrCall_reset(struct dmrRxCall *rx)
{
    memset(rx, 0, sizeof(*rx));
}

/**
 * Tell whether a call is one the filter takes.
 *
 * @param call: the call.
 * @param filter: which calls to take, NULL for every one.
 * @return true if it is taken.
 */
static bool wanted(const struct dmrVoiceCall *call,
                   const struct dmrRxFilter *filter)
{
    if ((filter == NULL) || filter->promiscuous)
        return true;

    if (call->groupCall)
        return call->dstId == filter->talkgroup;

    return call->dstId == filter->ownId;
}

enum dmrCallChange dmrCall_takeLc(struct dmrRxCall *rx, const uint8_t *lc,
                                  const uint8_t colorCode,
                                  const struct dmrRxFilter *filter,
                                  const long long now)
{
    struct dmrVoiceCall call;
    if (dmr_parseVoiceLc(lc, &call) == false)
        return DMR_CALL_IGNORED;

    if (wanted(&call, filter) == false)
        return DMR_CALL_IGNORED;

    rx->lastBurst = now;

    if (rx->active && (rx->call.srcId == call.srcId)
        && (rx->call.dstId == call.dstId)
        && (rx->call.groupCall == call.groupCall))
        return DMR_CALL_SAME;

    rx->call = call;
    rx->colorCode = colorCode;
    rx->active = true;

    return DMR_CALL_STARTED;
}

enum dmrCallChange dmrCall_takeTerminator(struct dmrRxCall *rx,
                                          const uint8_t *lc)
{
    struct dmrVoiceCall call;
    if ((dmr_parseVoiceLc(lc, &call) == false) || (rx->active == false))
        return DMR_CALL_IGNORED;

    if ((rx->call.srcId != call.srcId) || (rx->call.dstId != call.dstId))
        return DMR_CALL_IGNORED;

    rx->active = false;
    return DMR_CALL_ENDED;
}

bool dmrCall_takeVoice(struct dmrRxCall *rx, const long long now)
{
    if (rx->active == false)
        return false;

    rx->lastBurst = now;
    return true;
}

bool dmrCall_expired(const struct dmrRxCall *rx, const long long now,
                     const long long holdMs)
{
    return rx->active && ((now - rx->lastBurst) > holdMs);
}
