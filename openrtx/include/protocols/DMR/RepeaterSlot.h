/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_REPEATERSLOT_H
#define DMR_REPEATERSLOT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Following a repeater's downlink slot by slot, the way the stock firmwares
 * do. The downlink carries both timeslots back to back, sent by the repeater,
 * with a CACH between bursts whose TC bit tells the two apart. While searching,
 * the TC bit of each slot is shifted into a register; once it holds the
 * alternating pattern that ends on our timeslot, and the slot carries the
 * repeater's colour code, the receiver is aligned. From then on the slots
 * alternate, ours and the other, without looking at the TC bit again.
 *
 * The downlink is lost when the last sync stops being a repeater's, or after
 * four bad slots in a row; the search then starts over.
 */

/**
 * What a slot of the downlink showed, read at its boundary.
 */
struct dmrRptSample {
    bool repeater;     ///< The last sync came from a repeater, not a radio.
    bool checkFailed;  ///< The burst failed the baseband's check.
    uint8_t colorCode; ///< Colour code read with the slot.
    uint8_t tc;        ///< TC bit of the CACH, 0 or 1.
};

/**
 * What to do with the slot coming next.
 */
enum dmrRptAction {
    DMR_RPT_SEARCH, ///< Still searching: leave the receiver as it is.
    DMR_RPT_SKIP,   ///< The other timeslot: ignore it.
    DMR_RPT_TAKE,   ///< Our timeslot: receive it.
    DMR_RPT_LOST    ///< The downlink is gone: restart the receiver.
};

/**
 * State of the downlink as followed.
 */
struct dmrRptSlot {
    uint8_t timeslot;  ///< Our timeslot, 1 or 2.
    uint8_t colorCode; ///< The repeater's colour code.
    uint8_t tcBits;    ///< TC bits shifted in while searching.
    uint8_t misses;    ///< Bad slots in a row.
    bool aligned;      ///< Our timeslot has been found.
    bool ours;         ///< The slot coming next is ours.
};

/**
 * Start following a downlink from scratch.
 *
 * @param rpt: downlink state.
 * @param timeslot: our timeslot, 1 or 2.
 * @param colorCode: the repeater's colour code.
 */
void dmrRpt_reset(struct dmrRptSlot *rpt, const uint8_t timeslot,
                  const uint8_t colorCode);

/**
 * Take a slot boundary while no call is being received.
 *
 * @param rpt: downlink state.
 * @param sample: what the slot just gone showed.
 * @param slots: slot boundaries since the last one taken, 1 unless some were
 * missed.
 * @return what to do with the slot coming next.
 */
enum dmrRptAction dmrRpt_takeSlot(struct dmrRptSlot *rpt,
                                  const struct dmrRptSample *sample,
                                  const uint32_t slots);

/**
 * Move on by some slot boundaries, aligned, without judging the slots: what a
 * call in progress does, as it is judged on its own bursts.
 *
 * @param rpt: downlink state.
 * @param slots: slot boundaries since the last one taken.
 * @return DMR_RPT_TAKE if the slot coming next is ours, else DMR_RPT_SKIP.
 */
enum dmrRptAction dmrRpt_advance(struct dmrRptSlot *rpt, const uint32_t slots);

#ifdef __cplusplus
}
#endif

#endif /* DMR_REPEATERSLOT_H */
