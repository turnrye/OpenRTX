/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMRIRQ_H
#define DMRIRQ_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Handling of the interrupt lines the HR_C6000 uses to drive the DMR slot
 * timing.
 *
 * The chip raises a timeslot interrupt on every slot boundary, a system
 * interrupt when a frame event occurs and a transmit interrupt while sending.
 * The handlers only keep track of how many times each line fired, leaving both
 * the chip register access and the decoding to the thread consuming the
 * counters: the SPI interface towards the baseband is bitbanged with the
 * interrupts disabled and its chip select is taken through a mutex, so it
 * cannot be used from an interrupt context at all.
 */
struct dmrIrqCounters
{
    uint32_t timeslot;      ///< Timeslot interrupts, one per slot boundary.
    uint32_t system;        ///< System interrupts, one per frame event.
    uint32_t transmit;      ///< Transmit interrupts.
};

/**
 * Configure the interrupt lines and start counting the events.
 */
void dmrIrq_init();

/**
 * Stop handling the interrupt lines.
 */
void dmrIrq_terminate();

/**
 * Get the current value of the event counters.
 *
 * @param counters: destination for the counter values.
 */
void dmrIrq_getCounters(struct dmrIrqCounters *counters);

/**
 * Reset all the event counters to zero.
 */
void dmrIrq_clearCounters();

#ifdef __cplusplus
}
#endif

#endif /* DMRIRQ_H */
