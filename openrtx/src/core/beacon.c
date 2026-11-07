/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * beacon.c — when a position beacon is due.
 *
 * The adaptive method dead-reckons: it advances the last transmitted position
 * along the course and speed reported with it and compares that prediction —
 * what a listener extrapolates — against the current fix. All of it is fixed
 * point, so it runs on a target with no floating-point unit; the trigonometry
 * is a whole-degree cosine lookup, which is far finer than the position error
 * threshold needs.
 */

#include "core/beacon.h"

#include <string.h>

/* Metres per degree of latitude. A degree of longitude is this scaled by the
 * cosine of the latitude. One part in a thousand is well inside the accuracy a
 * beacon threshold in the hundreds of metres cares about. */
#define METERS_PER_DEGREE 111320

/* Cosine scaled by 4096, for whole degrees 0..90. */
#define TRIG_SCALE 4096

static const int16_t cosTable[91] = {
    4096, 4095, 4094, 4090, 4086, 4080, 4074, 4065, 4056, 4046, 4034, 4021,
    4006, 3991, 3974, 3956, 3937, 3917, 3896, 3873, 3849, 3824, 3798, 3770,
    3742, 3712, 3681, 3650, 3617, 3582, 3547, 3511, 3474, 3435, 3396, 3355,
    3314, 3271, 3228, 3183, 3138, 3091, 3044, 2996, 2946, 2896, 2845, 2793,
    2741, 2687, 2633, 2578, 2522, 2465, 2408, 2349, 2290, 2231, 2171, 2110,
    2048, 1986, 1923, 1860, 1796, 1731, 1666, 1600, 1534, 1468, 1401, 1334,
    1266, 1198, 1129, 1060, 991,  921,  852,  782,  711,  641,  570,  499,
    428,  357,  286,  214,  143,  71,   0
};

/**
 * Cosine of a whole-degree angle, scaled by TRIG_SCALE.
 */
static int32_t cosDeg(int deg)
{
    deg %= 360;
    if (deg < 0)
        deg += 360;

    if (deg <= 90)
        return cosTable[deg];
    if (deg <= 180)
        return -cosTable[180 - deg];
    if (deg <= 270)
        return -cosTable[deg - 180];
    return cosTable[360 - deg];
}

/**
 * Sine of a whole-degree angle, scaled by TRIG_SCALE.
 */
static int32_t sinDeg(int deg)
{
    return cosDeg(90 - deg);
}

/**
 * Integer square root of a 64-bit value.
 */
static uint32_t isqrt64(uint64_t value)
{
    uint64_t rem = value;
    uint64_t root = 0;
    uint64_t bit = (uint64_t)1 << 62;

    while (bit > rem)
        bit >>= 2;

    while (bit != 0) {
        if (rem >= root + bit) {
            rem -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }

    return (uint32_t)root;
}

/**
 * Distance, in metres, between the current fix and where a listener would
 * dead-reckon the station to be from its last report.
 */
static uint32_t drError(const struct beaconState *st,
                        const struct beaconFix *fix, uint32_t dt)
{
    /* Actual displacement since the last report, in metres. */
    int32_t dLatMicro = fix->latitude - st->lastTxLat;
    int32_t dLonMicro = fix->longitude - st->lastTxLon;

    int32_t latDeg = fix->latitude / 1000000;
    if (latDeg < 0)
        latDeg = -latDeg;
    int32_t cosLat = cosDeg((int)latDeg);

    int64_t actualNorth = (int64_t)dLatMicro * METERS_PER_DEGREE / 1000000;
    int64_t actualEast = (int64_t)dLonMicro * METERS_PER_DEGREE * cosLat
                       / TRIG_SCALE / 1000000;

    /* Displacement a listener extrapolates from the last course and speed. */
    int64_t dist = (int64_t)st->lastTxSpeed * dt * 1000 / 3600;
    int64_t predNorth = dist * cosDeg((int)st->lastTxCourse) / TRIG_SCALE;
    int64_t predEast = dist * sinDeg((int)st->lastTxCourse) / TRIG_SCALE;

    int64_t errEast = actualEast - predEast;
    int64_t errNorth = actualNorth - predNorth;

    return isqrt64((uint64_t)(errEast * errEast + errNorth * errNorth));
}

bool beacon_due(const struct beaconConfig *cfg, const struct beaconState *st,
                const struct beaconFix *fix, uint32_t now)
{
    if ((cfg == NULL) || (st == NULL) || (fix == NULL))
        return false;

    /* A beacon carries a position; without a fix there is nothing to send,
     * not even on an explicit request. */
    if (!fix->valid)
        return false;

    /* An explicit "beacon now" overrides the method and all timing. */
    if (st->forceNext)
        return true;

    if ((cfg->method == BEACON_OFF) || (cfg->method == BEACON_MANUAL))
        return false;

    /* The first beacon goes out as soon as there is a fix. */
    if (!st->everSent)
        return true;

    uint32_t dt = now - st->lastTxTime;

    /* Heartbeat: never stay silent longer than this, whatever the method. */
    if (dt >= cfg->maxInterval)
        return true;

    /* Rate floor: never beacon more often than this. */
    if (dt < cfg->minInterval)
        return false;

    switch (cfg->method) {
        case BEACON_INTERVAL:
            return true;

        case BEACON_ADAPTIVE:
            return drError(st, fix, dt) >= cfg->errThreshold;

        default:
            return false;
    }
}

void beacon_sent(struct beaconState *st, const struct beaconFix *fix,
                 uint32_t now)
{
    if ((st == NULL) || (fix == NULL))
        return;

    st->everSent = true;
    st->forceNext = false; // the request, if any, is satisfied
    st->lastTxTime = now;
    st->lastTxLat = fix->latitude;
    st->lastTxLon = fix->longitude;
    st->lastTxCourse = fix->course;
    st->lastTxSpeed = fix->speed;
}

void beacon_reset(struct beaconState *st)
{
    if (st != NULL)
        memset(st, 0, sizeof(*st));
}
