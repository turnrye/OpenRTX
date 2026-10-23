/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>
#include "protocols/DMR/RepeaterSlot.h"

static const uint8_t CC = 1;

static struct dmrRptSample sample(uint8_t tc, uint8_t cc = CC)
{
    struct dmrRptSample s;
    s.repeater = true;
    s.checkFailed = false;
    s.colorCode = cc;
    s.tc = tc;
    return s;
}

/* Feed slots of a downlink whose TC bit alternates, starting with tc. */
static std::vector<enum dmrRptAction> feed(struct dmrRptSlot *rpt, uint8_t tc,
                                           int count, uint8_t cc = CC)
{
    std::vector<enum dmrRptAction> out;
    for (int i = 0; i < count; i++) {
        struct dmrRptSample s = sample(tc, cc);
        out.push_back(dmrRpt_takeSlot(rpt, &s, 1));
        tc ^= 1;
    }
    return out;
}

/* Index of the first slot that aligned, or -1. */
static int alignedAt(const std::vector<enum dmrRptAction> &actions)
{
    for (size_t i = 0; i < actions.size(); i++)
        if (actions[i] != DMR_RPT_SEARCH)
            return (int)i;
    return -1;
}

TEST_CASE("DMR repeater downlink aligns on timeslot 2", "[dmr][repeater]")
{
    struct dmrRptSlot rpt;
    dmrRpt_reset(&rpt, 2, CC);

    // The TC bits 1, 0, 1, ... fill the register up to 0xAA on the 8th slot
    std::vector<enum dmrRptAction> a = feed(&rpt, 1, 12);
    REQUIRE(alignedAt(a) == 7);
    REQUIRE(rpt.aligned);

    // The slot after the aligning one is the other timeslot, then ours
    REQUIRE(a[7] == DMR_RPT_SKIP);
    REQUIRE(a[8] == DMR_RPT_TAKE);
    REQUIRE(a[9] == DMR_RPT_SKIP);
    REQUIRE(a[10] == DMR_RPT_TAKE);
}

TEST_CASE("DMR repeater downlink aligns on timeslot 1 one slot later",
          "[dmr][repeater]")
{
    struct dmrRptSlot rpt;
    dmrRpt_reset(&rpt, 1, CC);

    std::vector<enum dmrRptAction> a = feed(&rpt, 1, 12);
    REQUIRE(alignedAt(a) == 8);
    REQUIRE(a[8] == DMR_RPT_SKIP);
    REQUIRE(a[9] == DMR_RPT_TAKE);
}

TEST_CASE("DMR repeater downlink with another colour code is refused",
          "[dmr][repeater]")
{
    struct dmrRptSlot rpt;
    dmrRpt_reset(&rpt, 2, CC);

    std::vector<enum dmrRptAction> a = feed(&rpt, 1, 4, 5);
    REQUIRE(a[0] == DMR_RPT_SEARCH);
    REQUIRE(a[2] == DMR_RPT_SEARCH);
    REQUIRE(a[3] == DMR_RPT_LOST); // Four bad slots in a row
    REQUIRE_FALSE(rpt.aligned);
}

TEST_CASE("DMR repeater downlink aligns only on its colour code",
          "[dmr][repeater]")
{
    struct dmrRptSlot rpt;
    dmrRpt_reset(&rpt, 2, CC);
    feed(&rpt, 1, 7);

    // The TC bits match on this slot, but it carries another colour code
    struct dmrRptSample s = sample(0, 5);
    REQUIRE(dmrRpt_takeSlot(&rpt, &s, 1) == DMR_RPT_SEARCH);
    REQUIRE_FALSE(rpt.aligned);
}

TEST_CASE("DMR repeater downlink is lost on a radio's sync", "[dmr][repeater]")
{
    struct dmrRptSlot rpt;
    dmrRpt_reset(&rpt, 2, CC);
    feed(&rpt, 1, 10);
    REQUIRE(rpt.aligned);

    struct dmrRptSample s = sample(0);
    s.repeater = false;
    REQUIRE(dmrRpt_takeSlot(&rpt, &s, 1) == DMR_RPT_LOST);
    REQUIRE_FALSE(rpt.aligned);

    // The search starts over from an empty register
    REQUIRE(alignedAt(feed(&rpt, 1, 12)) == 7);
}

TEST_CASE("DMR repeater downlink is lost after four bad slots",
          "[dmr][repeater]")
{
    struct dmrRptSlot rpt;
    dmrRpt_reset(&rpt, 2, CC);
    feed(&rpt, 1, 9); // Aligned, the next slot is ours
    REQUIRE(rpt.ours);

    // Three bad slots keep the alternation going
    struct dmrRptSample bad = sample(0);
    bad.checkFailed = true;
    REQUIRE(dmrRpt_takeSlot(&rpt, &bad, 1) == DMR_RPT_SKIP);
    REQUIRE(dmrRpt_takeSlot(&rpt, &bad, 1) == DMR_RPT_TAKE);
    REQUIRE(dmrRpt_takeSlot(&rpt, &bad, 1) == DMR_RPT_SKIP);

    // A good one resets the count
    struct dmrRptSample good = sample(0);
    REQUIRE(dmrRpt_takeSlot(&rpt, &good, 1) == DMR_RPT_TAKE);
    for (int n = 0; n < 3; n++)
        REQUIRE(dmrRpt_takeSlot(&rpt, &bad, 1) != DMR_RPT_LOST);

    // The fourth in a row loses it
    REQUIRE(dmrRpt_takeSlot(&rpt, &bad, 1) == DMR_RPT_LOST);
    REQUIRE_FALSE(rpt.aligned);
}

TEST_CASE("DMR repeater downlink keeps its timeslot over a missed boundary",
          "[dmr][repeater]")
{
    struct dmrRptSlot rpt;
    dmrRpt_reset(&rpt, 2, CC);
    feed(&rpt, 1, 9);
    REQUIRE(rpt.ours);

    // Two boundaries at once swap the timeslots twice: ours comes next again
    struct dmrRptSample s = sample(0);
    REQUIRE(dmrRpt_takeSlot(&rpt, &s, 2) == DMR_RPT_TAKE);
    REQUIRE(dmrRpt_takeSlot(&rpt, &s, 3) == DMR_RPT_SKIP);
}

TEST_CASE("DMR repeater search starts over after a missed boundary",
          "[dmr][repeater]")
{
    struct dmrRptSlot rpt;
    dmrRpt_reset(&rpt, 2, CC);
    feed(&rpt, 1, 6); // Most of the way to aligning

    // A missed boundary: the TC bits before it no longer line up
    struct dmrRptSample s = sample(1);
    REQUIRE(dmrRpt_takeSlot(&rpt, &s, 2) == DMR_RPT_SEARCH);
    std::vector<enum dmrRptAction> a = feed(&rpt, 0, 8);
    REQUIRE(alignedAt(a) == 6); // A full run of 8 from the restart
}

TEST_CASE("DMR repeater call follows the alternation on its own",
          "[dmr][repeater]")
{
    struct dmrRptSlot rpt;
    dmrRpt_reset(&rpt, 2, CC);
    feed(&rpt, 1, 9);
    REQUIRE(rpt.ours);

    REQUIRE(dmrRpt_advance(&rpt, 1) == DMR_RPT_SKIP);
    REQUIRE(dmrRpt_advance(&rpt, 1) == DMR_RPT_TAKE);
    REQUIRE(dmrRpt_advance(&rpt, 2) == DMR_RPT_TAKE);
    REQUIRE(rpt.aligned);
}
