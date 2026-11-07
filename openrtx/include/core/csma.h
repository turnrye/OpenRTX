/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CSMA_H
#define CSMA_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * p-persistent CSMA channel access, as a KISS TNC does it. While the channel
 * is busy, wait; once it is clear, draw a random number each slot and
 * transmit when the draw is within the persistence, so stations that came
 * ready during another transmission do not all key up when it ends. A wait
 * that runs past its timeout transmits anyway, so a stuck carrier detect
 * cannot hold a frame forever. Time and carrier detect are passed in, so the
 * decision can be tested without a radio.
 */

/**
 * Channel-access state of one transmitter.
 */
struct csmaState {
    int64_t deferStart;    ///< Tick of the first attempt, -1 when idle.
    int64_t slotStart;     ///< Tick of the current slot, -1 between slots.
    uint32_t slotTime;     ///< Milliseconds between draws.
    uint32_t deferTimeout; ///< Milliseconds after which to transmit anyway.
    uint32_t rng;          ///< Generator state, never zero.
    uint8_t persist;       ///< Transmit when the draw, 0-255, is at most this.
};

/**
 * Set up the state of a transmitter.
 *
 * @param s: state to set up.
 * @param persist: persistence, 0-255.
 * @param slotTime: milliseconds between draws.
 * @param deferTimeout: milliseconds after which to transmit anyway.
 * @param seed: random seed; zero is replaced, since the generator cannot
 *              start from it.
 */
void csma_init(struct csmaState *s, uint8_t persist, uint32_t slotTime,
               uint32_t deferTimeout, uint32_t seed);

/**
 * Forget the wait in progress, keeping the random sequence, so the next
 * frame starts a fresh backoff.
 *
 * @param s: state to idle.
 */
void csma_idle(struct csmaState *s);

/**
 * Decide whether a waiting frame may be transmitted now. Call repeatedly
 * while the frame waits.
 *
 * @param s: channel-access state, updated in place.
 * @param busy: true if a carrier is detected.
 * @param now: current time in milliseconds.
 * @return true to transmit now.
 */
bool csma_mayTransmit(struct csmaState *s, bool busy, int64_t now);

#ifdef __cplusplus
}
#endif

#endif /* CSMA_H */
