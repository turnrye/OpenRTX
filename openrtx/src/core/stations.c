/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/stations.h"

#include <math.h>
#include <string.h>
#include <strings.h>

#ifndef CONFIG_STATIONS_MAX
#define CONFIG_STATIONS_MAX 64
#endif

/* Newest first; a touched record is moved to the front. */
static struct station table[CONFIG_STATIONS_MAX];
static size_t count;

static int indexOf(const char *call)
{
    for (size_t i = 0; i < count; i++) {
        if (strcasecmp(table[i].call, call) == 0)
            return (int)i;
    }

    return -1;
}

static void removeAt(size_t i)
{
    memmove(&table[i], &table[i + 1], (count - i - 1) * sizeof(table[0]));
    count--;
}

void stations_init(void)
{
    memset(table, 0, sizeof(table));
    count = 0;
}

struct station *stations_update(const char *call, uint8_t carrier, uint32_t now)
{
    if ((call == NULL) || (call[0] == '\0')
        || (strlen(call) >= STATION_CALL_LEN))
        return NULL;

    struct station rec;
    int i = indexOf(call);

    if (i >= 0) {
        rec = table[i];
        removeAt((size_t)i);
    } else {
        memset(&rec, 0, sizeof(rec));
        strcpy(rec.call, call);
        if (count == CONFIG_STATIONS_MAX)
            count--; /* the last is the oldest */
    }

    rec.carrier = carrier;
    rec.lastHeard = now;

    memmove(&table[1], &table[0], count * sizeof(table[0]));
    table[0] = rec;
    count++;

    return &table[0];
}

void stations_remove(const char *call)
{
    int i = (call != NULL) ? indexOf(call) : -1;
    if (i >= 0)
        removeAt((size_t)i);
}

void stations_expire(uint32_t now, uint32_t maxAge)
{
    /* Oldest last, so trimming from the end is enough. */
    while ((count > 0) && ((now - table[count - 1].lastHeard) > maxAge))
        count--;
}

const struct station *stations_find(const char *call)
{
    int i = (call != NULL) ? indexOf(call) : -1;
    return (i >= 0) ? &table[i] : NULL;
}

size_t stations_count(void)
{
    return count;
}

const struct station *stations_get(size_t idx)
{
    return (idx < count) ? &table[idx] : NULL;
}

uint32_t stations_distance(int32_t lat1, int32_t lon1, int32_t lat2,
                           int32_t lon2, uint16_t *bearing)
{
    const double rad = M_PI / 180.0e6;
    const double p1 = lat1 * rad, p2 = lat2 * rad;
    const double dl = (lon2 - lon1) * rad;

    /* Haversine; the Earth as a 6371 km sphere is within 0.5% */
    double a = sin((p2 - p1) / 2) * sin((p2 - p1) / 2)
             + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    double d = 2.0 * 6371000.0 * atan2(sqrt(a), sqrt(1.0 - a));

    if (bearing != NULL) {
        double y = sin(dl) * cos(p2);
        double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
        double b = atan2(y, x) * 180.0 / M_PI;
        if (b < 0)
            b += 360.0;
        *bearing = (uint16_t)((unsigned)lround(b) % 360u);
    }

    return (uint32_t)lround(d);
}
