/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/DMR/RepeaterSlot.h"

/* Bad slots in a row that mean the downlink is gone. */
#define RPT_MISS_LIMIT 4

/*
 * The TC bits that align on each timeslot: alternating, ending on 1 for
 * timeslot 1 and on 0 for timeslot 2.
 */
#define RPT_TC_TS1 0x55
#define RPT_TC_TS2 0xAA

/**
 * Forget the alignment and start searching again.
 *
 * @param rpt: downlink state.
 */
static void restart(struct dmrRptSlot *rpt)
{
    rpt->tcBits = 0;
    rpt->misses = 0;
    rpt->aligned = false;
    rpt->ours = false;
}

void dmrRpt_reset(struct dmrRptSlot *rpt, const uint8_t timeslot,
                  const uint8_t colorCode)
{
    rpt->timeslot = timeslot;
    rpt->colorCode = colorCode;
    restart(rpt);
}

enum dmrRptAction dmrRpt_advance(struct dmrRptSlot *rpt, const uint32_t slots)
{
    /* Every boundary swaps the two timeslots, missed ones included. */
    if ((slots & 1) != 0)
        rpt->ours = !rpt->ours;

    return rpt->ours ? DMR_RPT_TAKE : DMR_RPT_SKIP;
}

enum dmrRptAction dmrRpt_takeSlot(struct dmrRptSlot *rpt,
                                  const struct dmrRptSample *sample,
                                  const uint32_t slots)
{
    if (sample->repeater == false) {
        restart(rpt);
        return DMR_RPT_LOST;
    }

    bool bad = sample->checkFailed || (sample->colorCode != rpt->colorCode);
    if (bad) {
        rpt->misses += 1;
        if (rpt->misses >= RPT_MISS_LIMIT) {
            restart(rpt);
            return DMR_RPT_LOST;
        }
    } else {
        rpt->misses = 0;
    }

    if (rpt->aligned)
        return dmrRpt_advance(rpt, slots);

    /*
     * The TC bits only align when they come from consecutive slots: after a
     * missed boundary the search starts over.
     */
    if (slots != 1)
        rpt->tcBits = 0;

    if (rpt->tcBits == 0)
        rpt->tcBits = (sample->tc != 0) ? 3 : 0;
    else
        rpt->tcBits = (uint8_t)((rpt->tcBits << 1) | (sample->tc & 1));

    uint8_t target = (rpt->timeslot == 2) ? RPT_TC_TS2 : RPT_TC_TS1;
    if ((rpt->tcBits == target) && (sample->colorCode == rpt->colorCode)) {
        /* The slot coming next is the other timeslot, ours follows it. */
        rpt->aligned = true;
        rpt->ours = false;
        return DMR_RPT_SKIP;
    }

    return DMR_RPT_SEARCH;
}
