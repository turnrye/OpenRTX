/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/APRS/digipeater.h"
#include "protocols/APRS/packet.h"
#include "core/crc.h"
#include <ctype.h>
#include <string.h>

#define ADDR_LEN 7
#define ADDR_LAST 0x01
#define ADDR_SSID_MASK 0x1e
#define ADDR_SSID_SHIFT 1
#define ADDR_USED 0x80
#define MAX_ADDRESSES 10
#define CTRL_UI 0x03
#define PID_NONE 0xf0

/* ------------------------------------------------------------------------ *
 * The address field, on the raw bytes.
 */

/* Addresses in the frame, or 0 if the field is malformed. */
static uint8_t countAddresses(const uint8_t *frame, size_t len)
{
    uint8_t count = 0;

    for (size_t off = 0; (len - off) >= ADDR_LEN; off += ADDR_LEN) {
        if (count == MAX_ADDRESSES)
            return 0;
        count++;
        if (frame[off + ADDR_LEN - 1] & ADDR_LAST)
            return (count >= 2) ? count : 0;
    }

    return 0;
}

/* Callsign of an address as text, without its SSID. */
static void addrCall(const uint8_t *a, char *out)
{
    size_t j = 0;
    for (size_t k = 0; k < ADDR_LEN - 1; k++) {
        char c = (char)(a[k] >> 1);
        if (c != ' ')
            out[j++] = c;
    }
    out[j] = '\0';
}

static uint8_t addrSsid(const uint8_t *a)
{
    return (a[ADDR_LEN - 1] & ADDR_SSID_MASK) >> ADDR_SSID_SHIFT;
}

static void setSsid(uint8_t *a, uint8_t ssid)
{
    a[ADDR_LEN - 1] = (uint8_t)((a[ADDR_LEN - 1] & ~ADDR_SSID_MASK)
                                | ((ssid & 0x0f) << ADDR_SSID_SHIFT));
}

/* Whether an address names the station "CALL" or "CALL-SSID". */
static bool addrIs(const uint8_t *a, const char *station)
{
    struct aprsAddress want;
    char call[7];

    if ((station == NULL) || (station[0] == '\0')
        || (aprsAddr_fromStr(station, &want) != 0))
        return false;

    addrCall(a, call);
    return (strcmp(call, want.addr) == 0) && (addrSsid(a) == want.ssid);
}

/* Write a station into an address slot, keeping only the last-address bit. */
static void putAddress(uint8_t *a, const char *station, bool used)
{
    struct aprsAddress addr;
    size_t i = 0;

    if (aprsAddr_fromStr(station, &addr) != 0)
        memset(&addr, 0, sizeof(addr));

    uint8_t last = a[ADDR_LEN - 1] & ADDR_LAST;
    for (; (i < ADDR_LEN - 1) && (addr.addr[i] != '\0'); i++)
        a[i] = (uint8_t)(addr.addr[i] << 1);
    for (; i < ADDR_LEN - 1; i++)
        a[i] = (uint8_t)(' ' << 1);
    a[ADDR_LEN - 1] = (uint8_t)(0x60 | ((addr.ssid & 0x0f) << ADDR_SSID_SHIFT)
                                | last | (used ? ADDR_USED : 0));
}

/*
 * Whether an address is a generic "PREFIXn-N" alias: letters, one digit n,
 * SSID N with 1 <= N <= n.
 */
static bool genericAlias(const uint8_t *a, uint8_t *n, uint8_t *remaining)
{
    char call[7];
    addrCall(a, call);

    size_t len = strlen(call);
    if ((len < 2) || !isdigit((unsigned char)call[len - 1]))
        return false;
    for (size_t i = 0; i + 1 < len; i++) {
        if (!isupper((unsigned char)call[i]))
            return false;
    }

    *n = (uint8_t)(call[len - 1] - '0');
    *remaining = addrSsid(a);
    return (*n >= 1) && (*remaining >= 1) && (*remaining <= *n);
}

size_t ax25DigiTransform(const uint8_t *in, size_t len, uint8_t *out,
                         size_t cap, const char *me, const char *alias,
                         uint8_t maxN)
{
    if ((in == NULL) || (out == NULL) || (me == NULL) || (me[0] == '\0'))
        return 0;

    uint8_t count = countAddresses(in, len);
    if (count < 3)
        return 0; /* no via path */

    /* The first unused via address is the one to judge. */
    uint8_t i = 2;
    while ((i < count) && (in[i * ADDR_LEN + ADDR_LEN - 1] & ADDR_USED))
        i++;
    if (i == count)
        return 0;

    const uint8_t *via = &in[i * ADDR_LEN];
    uint8_t n, remaining;
    bool insert = false;

    if (addrIs(via, me) || addrIs(via, alias)) {
        /* Ours by name: replaced by our callsign, used. */
    } else if (genericAlias(via, &n, &remaining)) {
        if (n > maxN)
            return 0;
        insert = (remaining >= 2);
    } else {
        return 0;
    }

    if (insert && ((count == MAX_ADDRESSES) || (cap < len + ADDR_LEN)))
        return 0;
    if (cap < len)
        return 0;

    if (out != in)
        memmove(out, in, len);

    uint8_t *slot = &out[i * ADDR_LEN];
    if (insert) {
        /* Make room before the alias, then spend one of its hops. */
        memmove(slot + ADDR_LEN, slot, len - (size_t)i * ADDR_LEN);
        setSsid(slot + ADDR_LEN, (uint8_t)(remaining - 1));
        slot[ADDR_LEN - 1] &= (uint8_t)~ADDR_LAST;
        len += ADDR_LEN;
    }
    putAddress(slot, me, true);

    return len;
}

/* ------------------------------------------------------------------------ *
 * Duplicate accounting and the hold.
 */

/* Source, destination without its SSID, and the info field: what stays the
 * same on every hop. */
static uint32_t frameHash(const uint8_t *frame, size_t len, uint8_t count)
{
    uint32_t h = crc_hdlc(&frame[ADDR_LEN], ADDR_LEN);
    h = (h << 16) ^ crc_hdlc(frame, ADDR_LEN - 1);
    size_t info = (size_t)count * ADDR_LEN + 2;
    if (info < len)
        h ^= crc_hdlc(&frame[info], len - info) * 2654435761u;
    return (h != 0) ? h : 1;
}

static struct aprsDigiSeen *findSeen(struct aprsDigi *d, uint32_t hash,
                                     uint32_t now)
{
    for (int i = 0; i < APRS_DIGI_SEEN; i++) {
        struct aprsDigiSeen *s = &d->seen[i];
        if ((s->hash == hash) && (now < s->expires))
            return s;
    }
    return NULL;
}

static void releasePending(struct aprsDigi *d, struct aprsDigiSeen *s)
{
    if (s->pending >= 0) {
        d->pending[s->pending].len = 0;
        s->pending = -1;
    }
}

/* A record for a new frame: a free or expired one, else the oldest. */
static struct aprsDigiSeen *newSeen(struct aprsDigi *d, uint32_t now)
{
    struct aprsDigiSeen *oldest = &d->seen[0];

    for (int i = 0; i < APRS_DIGI_SEEN; i++) {
        struct aprsDigiSeen *s = &d->seen[i];
        if ((s->hash == 0) || (now >= s->expires)) {
            releasePending(d, s);
            memset(s, 0, sizeof(*s));
            s->pending = -1;
            return s;
        }
        if (s->expires < oldest->expires)
            oldest = s;
    }

    releasePending(d, oldest);
    memset(oldest, 0, sizeof(*oldest));
    oldest->pending = -1;
    return oldest;
}

static int freePending(const struct aprsDigi *d)
{
    for (int i = 0; i < APRS_DIGI_PENDING; i++) {
        if (d->pending[i].len == 0)
            return i;
    }
    return -1;
}

void aprsDigi_init(struct aprsDigi *d, const struct aprsDigiConfig *cfg)
{
    memset(d, 0, sizeof(*d));
    for (int i = 0; i < APRS_DIGI_SEEN; i++)
        d->seen[i].pending = -1;
    if (cfg != NULL)
        d->cfg = *cfg;
}

void aprsDigi_configure(struct aprsDigi *d, const struct aprsDigiConfig *cfg)
{
    d->cfg = *cfg;
    for (int i = 0; i < APRS_DIGI_SEEN; i++)
        releasePending(d, &d->seen[i]);
}

enum aprsDigiVerdict aprsDigi_offer(struct aprsDigi *d, const uint8_t *frame,
                                    size_t len, uint32_t now)
{
    if ((d == NULL) || (frame == NULL) || (len > APRS_PACLEN))
        return APRS_DIGI_IGNORED;

    uint8_t count = countAddresses(frame, len);
    if (count == 0)
        return APRS_DIGI_IGNORED;

    /* Only UI frames: a connected-mode frame has its own retries and
     * timers, and repeating it on APRS rules would break them. */
    size_t ctrl = (size_t)count * ADDR_LEN;
    if ((ctrl + 2 > len) || (frame[ctrl] != CTRL_UI)
        || (frame[ctrl + 1] != PID_NONE))
        return APRS_DIGI_IGNORED;

    /* Was it carried by a digipeater already, and did it pass through us? */
    bool repeated = false;
    bool loop = false;
    bool anyUnused = false;
    uint8_t used = 0;
    for (uint8_t i = 2; i < count; i++) {
        const uint8_t *via = &frame[i * ADDR_LEN];
        if (via[ADDR_LEN - 1] & ADDR_USED) {
            repeated = true;
            used++;
            if (addrIs(via, d->cfg.me))
                loop = true;
        } else {
            anyUnused = true;
        }
    }

    /* Duplicate accounting first, whatever we decide about the frame. */
    uint32_t hash = frameHash(frame, len, count);
    struct aprsDigiSeen *s = findSeen(d, hash, now);
    if (s != NULL) {
        if (repeated) {
            s->repeated++;
            if (s->pending >= 0) {
                /* Somebody else got there first: ours is not needed. */
                releasePending(d, s);
                d->stats.stoodDown++;
                return APRS_DIGI_STOOD_DOWN;
            }
        }
        return APRS_DIGI_DUPLICATE;
    }

    if (d->cfg.mode == APRS_DIGI_OFF)
        return APRS_DIGI_IGNORED;

    /* Never our own traffic, in either direction. */
    if (addrIs(&frame[ADDR_LEN], d->cfg.me) || loop) {
        if (loop)
            d->stats.refused++;
        return loop ? APRS_DIGI_REFUSED : APRS_DIGI_IGNORED;
    }

    if (!anyUnused)
        return APRS_DIGI_IGNORED; /* the path is spent */

    uint8_t maxN = (d->cfg.mode == APRS_DIGI_FILLIN) ? 1 : d->cfg.maxHops;
    bool directOnly = (d->cfg.mode == APRS_DIGI_FILLIN) || d->cfg.directOnly;

    uint8_t rewritten[APRS_PACLEN];
    size_t n = ax25DigiTransform(frame, len, rewritten, sizeof(rewritten),
                                 d->cfg.me, d->cfg.alias, maxN);
    if (n == 0)
        return APRS_DIGI_IGNORED;

    d->stats.heard++;

    if ((directOnly && repeated) || (used >= d->cfg.maxHops)) {
        d->stats.refused++;
        return APRS_DIGI_REFUSED;
    }

    int slot = freePending(d);
    if (slot < 0) {
        d->stats.refused++;
        return APRS_DIGI_REFUSED;
    }

    s = newSeen(d, now);
    s->hash = hash;
    s->expires = now + APRS_DIGI_DUP_WINDOW;
    s->release = now + d->cfg.delay;
    s->pending = (int8_t)slot;
    memcpy(d->pending[slot].frame, rewritten, n);
    d->pending[slot].len = (uint16_t)n;

    return APRS_DIGI_HELD;
}

size_t aprsDigi_due(struct aprsDigi *d, uint32_t now, uint8_t *out, size_t cap)
{
    if ((d == NULL) || (out == NULL))
        return 0;

    for (int i = 0; i < APRS_DIGI_SEEN; i++) {
        struct aprsDigiSeen *s = &d->seen[i];
        if ((s->hash == 0) || (s->pending < 0) || (now < s->release))
            continue;

        struct aprsDigiPending *p = &d->pending[s->pending];
        size_t n = p->len;
        if (n > cap)
            n = 0;
        else
            memcpy(out, p->frame, n);

        releasePending(d, s);
        /* The window runs from the transmission, as the algorithm says. */
        s->expires = now + APRS_DIGI_DUP_WINDOW;
        if (n > 0)
            d->stats.repeated++;
        return n;
    }

    return 0;
}
