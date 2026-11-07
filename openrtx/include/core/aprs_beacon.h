/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APRS_BEACON_H
#define APRS_BEACON_H

#include "core/beacon.h"
#include "protocols/APRS/packet.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file aprs_beacon.h
 * @brief Turn a position report into a transmitted APRS beacon.
 *
 * This is the APRS carrier for the scheduler in beacon.h: it takes a position
 * report the caller has already assembled, wraps it in an AX.25 frame and
 * hands it to the transmit path. It reads no global state — callsign, path and
 * the report all come in as arguments — so the settings layer and the GPS
 * layer stay on the caller's side of this boundary.
 */

/**
 * @brief Build and submit one APRS position beacon.
 *
 * Formats the report's info field, wraps it in a frame addressed from
 * @p callsign to the OpenRTX TOCALL along @p path, and submits it for
 * transmission. The frame's info field is bounded so the whole frame fits
 * within one packet however long the path is; a report that will not fit even
 * its coordinate is refused.
 *
 * A beacon is not sent under the factory callsign — an unconfigured radio must
 * not put N0CALL on the air — nor while a previous beacon is still being
 * transmitted, since the two would share one buffer.
 *
 * @param callsign: source address, "CALL" or "CALL-SSID"; refused if empty or
 *                  the factory N0CALL.
 * @param path: digipeater path, or NULL/"" for the default WIDE path.
 * @param report: the position report to transmit.
 * @return 0 if the beacon was submitted, -EINVAL for a bad callsign or a
 *         report that cannot be built, -EBUSY if a beacon is already in
 *         flight or the transmit slot is occupied, or another negative errno.
 */
int aprsBeacon_send(const char *callsign, const char *path,
                    const struct aprsPosReport *report);

/**
 * @brief Submit a position beacon in M17 packet mode.
 *
 * As aprsBeacon_send(), with the AX.25 frame carried in an M17 packet from
 * @p m17Callsign to everyone, and no digipeater path: M17 has none.
 *
 * @param m17Callsign: M17 source address; refused if empty or N0CALL.
 * @param callsign: AX.25 source address; refused if empty or N0CALL.
 * @param report: the position report to transmit.
 * @return as aprsBeacon_send().
 */
int aprsBeacon_sendM17(const char *m17Callsign, const char *callsign,
                       const struct aprsPosReport *report);

/**
 * @brief Everything a beacon tick needs beyond the position itself.
 *
 * The schedule and the presentation of a beacon are configuration; the
 * coordinate is live data. Keeping them apart lets a caller assemble this once
 * from settings and pass a fresh fix each tick.
 */
struct aprsBeaconParams {
    struct beaconConfig sched; /**< When to beacon.                    */
    const char *callsign;      /**< Source, "CALL" or "CALL-SSID".     */
    const char *path;          /**< Digipeater path, or NULL for WIDE. */
    char symbolTable;          /**< Symbol table id, e.g. '/'.         */
    char symbolCode;           /**< Symbol code, e.g. '>'.             */
    const char *comment;       /**< Trailing comment, or NULL.         */
    bool sendCourseSpeed;      /**< Include the fix's course/speed. */
    bool sendAltitude;         /**< Include the fix's altitude.     */
    uint8_t carrier;           /**< OPMODE_APRS, or OPMODE_M17 for */
                               /**< M17 packet mode.               */
    const char *m17Callsign;   /**< M17 source, for OPMODE_M17.    */
};

/**
 * @brief Run one beacon tick: decide, and if due, transmit and record.
 *
 * Asks the scheduler whether a beacon is due for @p fix at @p now; if so,
 * renders the fix into a position report per @p params and submits it, and on
 * success records the transmission so the next decision measures against it.
 * The transmit slot being busy is not a failure to record — the schedule is
 * left untouched so the next tick tries again.
 *
 * @param params: schedule and presentation, assembled from settings.
 * @param fix: the current position fix.
 * @param state: last-transmitted state, updated on a successful send.
 * @param now: the current time, in the seconds the scheduler expects.
 * @return 0 if a beacon was sent and recorded, -EAGAIN if none was due, or a
 *         negative errno from the transmit path (e.g. -EBUSY, -EINVAL), in
 *         which case nothing is recorded.
 */
int aprsBeacon_tick(const struct aprsBeaconParams *params,
                    const struct beaconFix *fix, struct beaconState *state,
                    uint32_t now);

/**
 * @brief Request an immediate beacon on the next tick.
 *
 * A one-shot "beacon now": the next aprsBeacon_tick() sends regardless of the
 * configured method or timing, as long as a position is available. If the
 * transmit slot is busy the request stands until a beacon actually goes out.
 * Safe to call from the same thread that runs the tick.
 */
void aprsBeacon_requestNow(void);

/**
 * @brief How long ago the last beacon was transmitted, for the status display.
 *
 * @param ageSecOut: written with seconds since the last beacon, if any.
 * @param nowSec: the current time in seconds.
 * @return true if a beacon has been sent since boot, false if none yet.
 */
bool aprsBeacon_lastSent(uint32_t *ageSecOut, uint32_t nowSec);

#ifdef __cplusplus
}
#endif

#endif /* APRS_BEACON_H */
