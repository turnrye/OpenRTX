/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * aprs_beacon.c — the APRS carrier for position beacons.
 *
 * Wraps an already-assembled position report in an AX.25 frame and submits it
 * for transmission, mirroring how the message source formats and hands off a
 * frame. The frame lives in a single static buffer because the transmit path
 * reads it asynchronously: a new beacon cannot reuse the buffer until the last
 * one has left it, which is what the in-flight check enforces.
 */

#include "core/aprs_beacon.h"
#include "rtx/rtx.h"
#include "protocols/M17/AprsPacket.h"

#include <errno.h>
#include <string.h>

/* The frame in flight, and the descriptor the transmit path acts on. Static
 * because the descriptor must outlive this call: the RTX task reads it after
 * aprsBeacon_send() has returned. */
static uint8_t beaconFrame[APRS_PACLEN];
static struct pktDesc beaconDesc = { PKT_STATUS_IDLE, beaconFrame, 0, 0 };

/* The same beacon in M17 packet mode, laid out as struct m17Packet: source,
 * destination, then the APRS payload (see protocols/M17/AprsPacket.h). */
static struct __attribute__((packed)) {
    char src[10];
    char dst[10];
    uint8_t payload[APRS_PACLEN + 64]; /* a TNC2 line outgrows its frame */
} m17Beacon;
static struct pktDesc m17Desc = { PKT_STATUS_IDLE, &m17Beacon, 0, 0 };

/* Set by aprsBeacon_requestNow(), consumed into the beacon state on the next
 * tick. A latch rather than an immediate send so the request is served from
 * the one place that owns the schedule. */
static bool forcePending = false;

/* Last beacon that actually went out, for the status display. */
static bool everSent = false;
static uint32_t lastTxSec = 0;

void aprsBeacon_requestNow(void)
{
    forcePending = true;
}

bool aprsBeacon_lastSent(uint32_t *ageSecOut, uint32_t nowSec)
{
    if (!everSent)
        return false;

    if (ageSecOut != NULL)
        *ageSecOut = nowSec - lastTxSec;

    return true;
}

/**
 * An unconfigured or factory callsign, with or without an SSID suffix.
 */
static bool isDefaultCall(const char *call)
{
    if ((call == NULL) || (call[0] == '\0'))
        return true;

    if ((strncmp(call, "N0CALL", 6) == 0)
        && ((call[6] == '\0') || (call[6] == '-')))
        return true;

    return false;
}

/**
 * Number of digipeater addresses a path spells out.
 */
static size_t pathHops(const char *path)
{
    if ((path == NULL) || (path[0] == '\0'))
        return 0;

    size_t hops = 1;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == ',')
            hops++;
    }

    return hops;
}

int aprsBeacon_send(const char *callsign, const char *path,
                    const struct aprsPosReport *report)
{
    if ((report == NULL) || isDefaultCall(callsign))
        return -EINVAL;

    const char *usePath = ((path == NULL) || (path[0] == '\0')) ?
                              APRS_DEFAULT_PATH :
                              path;

    /* Leave a beacon that is still being transmitted alone; its frame is in
     * the shared buffer this call would overwrite. */
    if ((beaconDesc.status == PKT_STATUS_SUBMITTED)
        || (m17Desc.status == PKT_STATUS_SUBMITTED))
        return -EBUSY;

    /* Bound the info field so the whole frame fits: a source and destination
     * address, one per digipeater hop, seven bytes each, plus the control and
     * protocol-identifier bytes. */
    const size_t overhead = 7u * (2u + pathHops(usePath)) + 2u;
    if ((overhead + 20u) > APRS_PACLEN)
        return -EINVAL;
    const size_t maxInfo = APRS_PACLEN - overhead;

    char info[APRS_PACLEN];
    size_t infoLen = aprsPosFormat(info, maxInfo + 1, report);
    if (infoLen == 0)
        return -EINVAL;

    ssize_t frameLen = aprsFrame_build(beaconFrame, sizeof(beaconFrame),
                                       APRS_TOCALL, callsign, usePath, info,
                                       infoLen);
    if (frameLen < 0)
        return (int)frameLen;

    beaconDesc.buffer = beaconFrame;
    beaconDesc.size = (size_t)frameLen;
    beaconDesc.status = PKT_STATUS_IDLE;

    /* rtx_addPacketTx() takes the descriptor from here: it moves it to
     * SUBMITTED, and on a refusal moves it back to IDLE, so the buffer is left
     * free exactly when the frame did not go out. */
    return rtx_addPacketTx(&beaconDesc);
}

int aprsBeacon_sendM17(const char *m17Callsign, const char *callsign,
                       const struct aprsPosReport *report)
{
    if ((report == NULL) || isDefaultCall(callsign)
        || isDefaultCall(m17Callsign))
        return -EINVAL;

    if ((beaconDesc.status == PKT_STATUS_SUBMITTED)
        || (m17Desc.status == PKT_STATUS_SUBMITTED))
        return -EBUSY;

    /* Room for the source and destination, with no path, and the control
     * and protocol-identifier bytes. */
    const size_t maxInfo = APRS_PACLEN - (7u * 2u + 2u);
    char info[APRS_PACLEN];
    size_t infoLen = aprsPosFormat(info, maxInfo + 1, report);
    if (infoLen == 0)
        return -EINVAL;

    ssize_t frameLen = aprsFrame_build(beaconFrame, sizeof(beaconFrame),
                                       APRS_TOCALL, callsign, NULL, info,
                                       infoLen);
    if (frameLen < 0)
        return (int)frameLen;

    size_t payloadLen = m17_aprsFormat(beaconFrame, (size_t)frameLen,
                                       m17Beacon.payload,
                                       sizeof(m17Beacon.payload));
    if (payloadLen == 0)
        return -EINVAL;

    memset(m17Beacon.src, 0, sizeof(m17Beacon.src));
    memset(m17Beacon.dst, 0, sizeof(m17Beacon.dst));
    strncpy(m17Beacon.src, m17Callsign, sizeof(m17Beacon.src) - 1);
    strncpy(m17Beacon.dst, "ALL", sizeof(m17Beacon.dst) - 1);

    m17Desc.size = sizeof(m17Beacon.src) + sizeof(m17Beacon.dst) + payloadLen;
    m17Desc.status = PKT_STATUS_IDLE;
    return rtx_addPacketTx(&m17Desc);
}

int aprsBeacon_tick(const struct aprsBeaconParams *params,
                    const struct beaconFix *fix, struct beaconState *state,
                    uint32_t now)
{
    if ((params == NULL) || (fix == NULL) || (state == NULL))
        return -EINVAL;

    /* Fold a pending "beacon now" into the state; beacon_due() honours it and
     * beacon_sent() clears it once a beacon actually leaves. */
    if (forcePending) {
        state->forceNext = true;
        forcePending = false;
    }

    if (!beacon_due(&params->sched, state, fix, now))
        return -EAGAIN;

    struct aprsPosReport report;
    report.latitude = fix->latitude;
    report.longitude = fix->longitude;
    report.symbolTable = params->symbolTable;
    report.symbolCode = params->symbolCode;
    report.hasCourseSpeed = params->sendCourseSpeed;
    report.course = fix->course;
    report.speed = fix->speed;
    report.hasAltitude = params->sendAltitude;
    report.altitude = fix->altitude;
    report.comment = params->comment;

    int ret = (params->carrier == OPMODE_M17) ?
                  aprsBeacon_sendM17(params->m17Callsign, params->callsign,
                                     &report) :
                  aprsBeacon_send(params->callsign, params->path, &report);

    /* Only a beacon that actually went out advances the reference point; a
     * busy slot or a refused frame leaves the schedule so the next tick
     * retries. */
    if (ret == 0) {
        beacon_sent(state, fix, now);
        everSent = true;
        lastTxSec = now;
    }

    return ret;
}
