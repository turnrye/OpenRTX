/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef BEACON_H
#define BEACON_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file beacon.h
 * @brief Protocol-agnostic decision of when a position beacon is due.
 *
 * This is the scheduling core, kept separate from everything that has a side
 * effect. It answers one question — should a beacon be transmitted now — from
 * plain values, so it can be tested exhaustively without a radio, a GPS, or a
 * clock. The current time is passed in rather than read, and the "last
 * transmitted" state is owned by the caller, so the same logic drives any
 * carrier the radio happens to be running.
 */

/**
 * @brief How beacons are scheduled.
 */
enum beaconMethod {
    BEACON_OFF = 0,  /**< Never beacon automatically.                    */
    BEACON_MANUAL,   /**< Only when the operator asks; never on a timer. */
    BEACON_INTERVAL, /**< At a fixed time interval.                      */
    BEACON_ADAPTIVE, /**< When the position has diverged enough to send. */
};

/**
 * @brief Scheduling parameters, the same for every carrier.
 *
 * Times are in seconds and distances in metres. @c minInterval is the rate
 * floor honoured by every automatic method; @c maxInterval is the heartbeat
 * ceiling, the longest silence allowed before a beacon goes out regardless of
 * movement; @c errThreshold is the position error that triggers an adaptive
 * beacon.
 */
struct beaconConfig {
    enum beaconMethod method;
    uint32_t minInterval;
    uint32_t maxInterval;
    uint32_t errThreshold;
};

/**
 * @brief A position fix, in the units the GPS layer reports.
 *
 * @c course is degrees from true north and @c speed is km/h; both describe the
 * instant of the fix and are only consulted by the adaptive method.
 */
/** A GPS fix older than this is stale: the beacon waits for a fresh one. */
#define BEACON_FIX_MAX_AGE_MS 30000u

struct beaconFix {
    bool valid;        /**< A usable position is present.        */
    int32_t latitude;  /**< Millionths of a degree, +N.         */
    int32_t longitude; /**< Millionths of a degree, +E.         */
    uint16_t course;   /**< Course over ground, degrees 0-359.  */
    uint16_t speed;    /**< Ground speed, km/h.                 */
    int16_t altitude;  /**< Altitude, metres MSL. Reported, not */
                       /**< used for scheduling.                */
};

/**
 * @brief What was last transmitted, so divergence can be measured against it.
 *
 * The caller stores the position it actually put on the air — after any
 * position-ambiguity masking — so the adaptive method compares against what a
 * listener received, not against the raw fix.
 */
struct beaconState {
    bool everSent;         /**< A beacon has gone out since the last reset. */
    bool forceNext;        /**< Send once now, overriding method and timing. */
    uint32_t lastTxTime;   /**< Time of that beacon, same clock as `now`.   */
    int32_t lastTxLat;     /**< Its latitude, millionths of a degree.       */
    int32_t lastTxLon;     /**< Its longitude, millionths of a degree.      */
    uint16_t lastTxCourse; /**< Its reported course, degrees.               */
    uint16_t lastTxSpeed;  /**< Its reported speed, km/h.                    */
};

/**
 * @brief Decide whether an automatic beacon is due.
 *
 * Returns false for the off and manual methods, and whenever there is no
 * valid fix — a beacon without a position is nothing to send. The first
 * automatic beacon goes out as soon as a fix is available. After that a
 * beacon is due once @c minInterval has elapsed and either the interval has
 * come round (interval method) or the position has diverged past
 * @c errThreshold (adaptive method), and unconditionally once @c maxInterval
 * has elapsed.
 *
 * The adaptive method measures divergence by dead reckoning: it advances the
 * last transmitted position along the course and speed that were reported with
 * it and compares that prediction — what a listener would extrapolate — with
 * the current fix. Straight-line travel therefore does not trigger a beacon,
 * while stopping or turning does.
 *
 * @param cfg: scheduling parameters.
 * @param st: last-transmitted state.
 * @param fix: the current position fix.
 * @param now: the current time, in the same seconds as @c st->lastTxTime.
 * @return true if a beacon should be transmitted now.
 */
bool beacon_due(const struct beaconConfig *cfg, const struct beaconState *st,
                const struct beaconFix *fix, uint32_t now);

/**
 * @brief Record that a beacon was transmitted.
 *
 * Stores the fix and time as the new reference point for the next decision.
 * The caller passes the position it actually transmitted, so masking applied
 * before transmission is reflected here too.
 *
 * @param st: state to update.
 * @param fix: the position that was transmitted.
 * @param now: the time it was transmitted.
 */
void beacon_sent(struct beaconState *st, const struct beaconFix *fix,
                 uint32_t now);

/**
 * @brief Clear the last-transmitted state.
 *
 * Used when the operating mode changes, so the next beacon is treated as a
 * first beacon rather than measured against a stale reference.
 *
 * @param st: state to clear.
 */
void beacon_reset(struct beaconState *st);

#ifdef __cplusplus
}
#endif

#endif /* BEACON_H */
