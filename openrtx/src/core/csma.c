/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/csma.h"

/* Marks a timer that is not running: 0 is a valid tick. */
#define NOT_STARTED (-1)

/* The draw is the low byte of the generator, compared against persist. */
#define DRAW_MASK 0xff

/*
 * Next value of a xorshift32 generator.
 */
static uint32_t nextRandom(struct csmaState *s)
{
    uint32_t x = s->rng;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng = x;

    return x;
}

void csma_init(struct csmaState *s, uint8_t persist, uint32_t slotTime,
               uint32_t deferTimeout, uint32_t seed)
{
    s->persist = persist;
    s->slotTime = slotTime;
    s->deferTimeout = deferTimeout;
    s->rng = (seed != 0) ? seed : 1;
    csma_idle(s);
}

void csma_idle(struct csmaState *s)
{
    s->deferStart = NOT_STARTED;
    s->slotStart = NOT_STARTED;
}

bool csma_mayTransmit(struct csmaState *s, bool busy, int64_t now)
{
    if (s->deferStart == NOT_STARTED)
        s->deferStart = now;

    if ((now - s->deferStart) >= s->deferTimeout)
        return true;

    // A busy channel ends the slot: the next draw comes once it clears
    if (busy) {
        s->slotStart = NOT_STARTED;
        return false;
    }

    if ((s->slotStart != NOT_STARTED) && ((now - s->slotStart) < s->slotTime))
        return false;

    s->slotStart = now;
    return (nextRandom(s) & DRAW_MASK) <= s->persist;
}
