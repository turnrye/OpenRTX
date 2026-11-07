/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/aprs_traffic.h"

#include <stdio.h>
#include <string.h>

#ifndef CONFIG_APRS_TRAFFIC_ENTRIES
#define CONFIG_APRS_TRAFFIC_ENTRIES 32
#endif

/* The digipeaters' duplicate window. */
#define DUP_WINDOW 30u

static struct aprsTrafficEntry entries[CONFIG_APRS_TRAFFIC_ENTRIES];
static size_t head; /* next slot to write */
static size_t count;

/* FNV-1a over the sender and the info field. */
static uint32_t hashPacket(const struct aprsPacket *pkt, const char *sender)
{
    uint32_t h = 2166136261u;

    for (const char *s = sender; *s != '\0'; s++)
        h = (h ^ (uint8_t)*s) * 16777619u;
    for (size_t i = 0; i < pkt->infoLen; i++)
        h = (h ^ (uint8_t)pkt->info[i]) * 16777619u;

    return h;
}

void aprsTraffic_init(void)
{
    memset(entries, 0, sizeof(entries));
    head = 0;
    count = 0;
}

void aprsTraffic_record(const struct aprsPacket *pkt, uint8_t flags,
                        uint32_t now)
{
    if ((pkt == NULL) || (pkt->addressesLen < 2))
        return;

    struct aprsTrafficEntry *e = &entries[head];
    memset(e, 0, sizeof(*e));

    aprsAddr_toStr(&pkt->addresses[1], e->sender, sizeof(e->sender));
    e->hash = hashPacket(pkt, e->sender);
    e->heard = now;
    e->type = (uint8_t)pkt->type;
    e->flags = flags;

    for (size_t i = 0; i < count; i++) {
        const struct aprsTrafficEntry *p = aprsTraffic_get(i);
        if ((now - p->heard) > DUP_WINDOW)
            break;
        if ((p->hash == e->hash) && !(p->flags & APRS_TRAFFIC_TX)) {
            e->flags |= APRS_TRAFFIC_DUP;
            break;
        }
    }

    char dest[APRS_ADDR_STR_LEN];
    char path[64];
    aprsAddr_toStr(&pkt->addresses[0], dest, sizeof(dest));
    aprsPath_toStr(pkt, path, sizeof(path));
    snprintf(e->line, sizeof(e->line), "%s>%s%s%s:%.*s", e->sender, dest,
             (path[0] != '\0') ? "," : "", path, (int)pkt->infoLen, pkt->info);
    aprsSanitize(e->line);

    head = (head + 1) % CONFIG_APRS_TRAFFIC_ENTRIES;
    if (count < CONFIG_APRS_TRAFFIC_ENTRIES)
        count++;
}

size_t aprsTraffic_count(void)
{
    return count;
}

const struct aprsTrafficEntry *aprsTraffic_get(size_t idx)
{
    if (idx >= count)
        return NULL;

    size_t pos = (head + CONFIG_APRS_TRAFFIC_ENTRIES - 1 - idx)
               % CONFIG_APRS_TRAFFIC_ENTRIES;
    return &entries[pos];
}
