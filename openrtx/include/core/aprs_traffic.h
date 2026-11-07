/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APRS_TRAFFIC_H
#define APRS_TRAFFIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "protocols/APRS/packet.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Log of every APRS packet heard or sent, newest first: the monitor view
 * the inbox is not. Entries are fixed size and the oldest is dropped when
 * the log is full. UI thread only.
 */

/** Longest TNC2 line kept per entry, including the NUL. */
#define APRS_TRAFFIC_LINE_LEN 100

/** Flags of an entry. */
enum aprsTrafficFlags {
    APRS_TRAFFIC_TX = 1,     /**< Sent by this station.                     */
    APRS_TRAFFIC_FOR_ME = 2, /**< A message addressed to this station.      */
    APRS_TRAFFIC_DUP = 4,    /**< Same sender and content within 30 s.       */
    APRS_TRAFFIC_GROUP = 8,  /**< A bulletin or other group message.        */
    APRS_TRAFFIC_EMERGENCY = 16,   /**< Raised an emergency.                  */
    APRS_TRAFFIC_M17 = 32,         /**< Carried in M17 packet mode.           */
    APRS_TRAFFIC_THIRD_PARTY = 64, /**< Relayed for another, unwrapped.     */
};

struct aprsTrafficEntry {
    uint32_t heard; /**< When, in seconds of system tick.           */
    uint32_t hash;  /**< Of sender and info, for duplicate marking. */
    char sender[APRS_ADDR_STR_LEN];
    uint8_t type;   /**< enum aprsType.          */
    uint8_t flags;  /**< enum aprsTrafficFlags.  */
    char line[APRS_TRAFFIC_LINE_LEN]; /**< "SRC>DST,PATH:info", cut to fit. */
};

/** Empty the log. */
void aprsTraffic_init(void);

/**
 * Add a packet. APRS_TRAFFIC_DUP is set here when the same sender sent the
 * same info within the last 30 seconds.
 *
 * @param pkt: parsed packet.
 * @param flags: APRS_TRAFFIC_TX and APRS_TRAFFIC_FOR_ME as applicable.
 * @param now: current time, seconds.
 */
void aprsTraffic_record(const struct aprsPacket *pkt, uint8_t flags,
                        uint32_t now);

/** Number of entries held. */
size_t aprsTraffic_count(void);

/**
 * Get an entry by position, newest first. Valid until the next record.
 *
 * @return the entry, or NULL if idx is out of range.
 */
const struct aprsTrafficEntry *aprsTraffic_get(size_t idx);

#ifdef __cplusplus
}
#endif

#endif /* APRS_TRAFFIC_H */
