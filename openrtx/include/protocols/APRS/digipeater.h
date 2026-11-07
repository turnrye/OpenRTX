/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APRS_DIGIPEATER_H
#define APRS_DIGIPEATER_H

#include "protocols/APRS/constants.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A fill-in digipeater, after the APRS Foundation's digipeater algorithm
 * with the defences aprx learned on real networks: it repeats only what it
 * heard directly, holds each frame a few seconds and sends it only if nobody
 * else repeated it meanwhile ("viscous" delay), refuses paths asking for or
 * having taken too many hops, and never repeats its own traffic or a frame
 * it already carried.
 *
 * The frame transform is plain AX.25 and reads no APRS policy; the policy
 * sits in aprsDigi_offer(). Nothing here does I/O: the caller feeds frames
 * in and takes due frames out.
 */

/** Frames remembered for duplicate suppression. */
#define APRS_DIGI_SEEN 32
/** Frames that can be held at once awaiting their release. */
#define APRS_DIGI_PENDING 4
/** Seconds a frame stays known, per the algorithm. */
#define APRS_DIGI_DUP_WINDOW 30u
/** Longest hold before a frame is given up on. */
#define APRS_DIGI_MAX_DELAY 9

enum aprsDigiMode {
    APRS_DIGI_OFF = 0,
    APRS_DIGI_FILLIN, /**< WIDE1-1 only, direct only, 5 s hold.     */
    APRS_DIGI_CUSTOM, /**< WIDEn-N up to the hop limit, own settings. */
};

struct aprsDigiConfig {
    uint8_t mode;    /**< enum aprsDigiMode.                          */
    uint8_t delay;   /**< Seconds held before sending; 0 = at once.   */
    uint8_t maxHops; /**< Largest n served in WIDEn-N, and the most   */
                     /**< used hops a path may already carry.         */
    bool directOnly; /**< Repeat only frames heard from their source. */
    char me[10];     /**< "CALL" or "CALL-SSID".                      */
    char alias[10];  /**< A second name answered to, or "".           */
};

/** What became of a frame offered to the digipeater. */
enum aprsDigiVerdict {
    APRS_DIGI_IGNORED = 0, /**< Not ours to repeat.                        */
    APRS_DIGI_HELD,        /**< Rewritten and waiting for its release.     */
    APRS_DIGI_DUPLICATE,   /**< Seen within the window; nothing to do.     */
    APRS_DIGI_STOOD_DOWN,  /**< Someone else repeated a frame we held.     */
    APRS_DIGI_REFUSED,     /**< Eligible, but refused by policy or limits. */
};

struct aprsDigiStats {
    uint32_t heard;     /**< Frames we would repeat.                       */
    uint32_t repeated;  /**< Frames we did repeat.                         */
    uint32_t stoodDown; /**< Frames another digipeater repeated first.     */
    uint32_t refused;   /**< Eligible frames refused: loop, limit, dupe.   */
};

struct aprsDigiSeen {
    uint32_t hash;    /**< Of source, destination and info. 0 = unused.   */
    uint32_t expires; /**< Seconds; the record is free once past.         */
    uint32_t release; /**< Seconds the held frame may go out at.          */
    int8_t pending;   /**< Index into the pending slots, -1 if none.      */
    uint8_t repeated; /**< Copies heard already carried by a digipeater.  */
};

struct aprsDigiPending {
    uint16_t len; /**< Frame length, 0 when the slot is free. */
    uint8_t frame[APRS_PACLEN];
};

struct aprsDigi {
    struct aprsDigiConfig cfg;
    struct aprsDigiStats stats;
    struct aprsDigiSeen seen[APRS_DIGI_SEEN];
    struct aprsDigiPending pending[APRS_DIGI_PENDING];
};

/**
 * Rewrite the address field of an AX.25 UI frame as a digipeater that
 * answers to @p me and @p alias would, for the first unused via address.
 *
 * A via equal to @p me or @p alias is replaced by @p me, marked used. A
 * generic "PREFIXn-N" via with N >= 2 has N decremented and @p me inserted
 * before it, marked used; with N == 1 it is replaced by @p me. Nothing
 * past the address field is touched.
 *
 * @param in: frame without its FCS.
 * @param len: its length.
 * @param out: destination for the rewritten frame; may equal @p in.
 * @param cap: size of out, at least len + 7.
 * @param me: this station, "CALL" or "CALL-SSID".
 * @param alias: another name to answer to, or NULL / "".
 * @param maxN: largest n served in a generic PREFIXn-N via.
 * @return the rewritten length, 0 if the frame is not one to repeat: no
 *         unused via, no match, a generic via above maxN, a malformed
 *         address field, or no room for an insertion.
 */
size_t ax25DigiTransform(const uint8_t *in, size_t len, uint8_t *out,
                         size_t cap, const char *me, const char *alias,
                         uint8_t maxN);

/** Start with @p cfg and nothing remembered. */
void aprsDigi_init(struct aprsDigi *d, const struct aprsDigiConfig *cfg);

/** Take a new configuration; held frames are dropped. */
void aprsDigi_configure(struct aprsDigi *d, const struct aprsDigiConfig *cfg);

/**
 * Offer a frame heard on the air.
 *
 * Duplicate accounting comes first: a copy that shows the frame was
 * repeated by another station cancels the copy we hold. Then, if the frame
 * is ours to repeat, it is rewritten and held until @p now + delay.
 *
 * @param d: digipeater state.
 * @param frame: AX.25 frame without its FCS.
 * @param len: its length.
 * @param now: current time, seconds.
 * @return what became of it.
 */
enum aprsDigiVerdict aprsDigi_offer(struct aprsDigi *d, const uint8_t *frame,
                                    size_t len, uint32_t now);

/**
 * Take a frame whose hold has expired, if there is one.
 *
 * The frame leaves the hold and counts as repeated; its record stays in
 * the duplicate window.
 *
 * @param d: digipeater state.
 * @param now: current time, seconds.
 * @param out: destination for the frame.
 * @param cap: size of out.
 * @return frame length, or 0 if nothing is due.
 */
size_t aprsDigi_due(struct aprsDigi *d, uint32_t now, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* APRS_DIGIPEATER_H */
