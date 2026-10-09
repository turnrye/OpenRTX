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
struct dmrIrqCounters {
    uint32_t timeslot; ///< Timeslot interrupts, one per slot boundary.
    uint32_t system;   ///< System interrupts, one per frame event.
    uint32_t transmit; ///< Transmit interrupts.
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

/**
 * Tell the interrupt handlers that the thread consuming the events is still
 * running. Has to be called regularly: the handlers shut the interrupt lines
 * down when too many events pile up between two calls, which keeps a line
 * firing continuously from starving every thread in the radio.
 */
void dmrIrq_feedWatchdog();

/**
 * Check whether the interrupt lines have been shut down because they were
 * firing far more often than a slot boundary ever could.
 *
 * @return true if the lines have been shut down.
 */
bool dmrIrq_stormDetected();

/**
 * Get the colour code of the most recent frame event, as read by the system
 * interrupt handler straight from the chip. Reading it there catches the value
 * at the instant of the event: the colour code register is only meaningful for
 * a moment and a read from the consuming thread arrives too late to see it.
 *
 * @return the last received colour code, or 0xFF before any has been read.
 */
uint8_t dmrIrq_getRxColorCode();

/**
 * Get the status register of the most recent frame event, as read by the
 * system interrupt handler straight from the chip. Like the colour code this
 * catches the value at the instant of the event, which a read from the
 * consuming thread arrives too late to see. Bits 7..4 hold the data type or
 * voice frame sequence number, bit 2 the CRC result, bits 1..0 the sync class
 * (1 = voice).
 *
 * @return the last received burst status, or 0 before any has been read.
 */
uint8_t dmrIrq_getRxBurstStatus();

/**
 * Tell the interrupt handlers whether a thread is currently accessing the chip
 * over the SPI bus. While it is, the system interrupt handler skips its own
 * register read rather than driving the bus at the same time; it picks the
 * colour code up again on the next event. Must bracket every thread-side chip
 * access made while the interrupts are live.
 *
 * @param busy: true around a thread-side transaction, false otherwise.
 */
void dmrIrq_setBusBusy(bool busy);

/**
 * Events the interrupt lines signal to a thread waiting on them.
 */
#define DMR_IRQ_EVENT_TIMESLOT 0x01 ///< Slot boundary.
#define DMR_IRQ_EVENT_SYSTEM 0x02   ///< Frame event, flags in 0x82.
#define DMR_IRQ_EVENT_WAKE 0x04     ///< Woken by dmrIrq_wakeWaiter.

/**
 * Wait until the interrupt lines signal an event, on the platforms which
 * handle reception from a thread of their own: the handlers only record the
 * event and wake the thread, which then does all the chip access through the
 * normal, mutex protected, register accessors. Only one thread may wait.
 *
 * @return the events signalled since the previous call, DMR_IRQ_EVENT_* bits.
 */
uint32_t dmrIrq_waitEvents();

/**
 * Get the events signalled since the previous call without waiting.
 *
 * @return the pending events, DMR_IRQ_EVENT_* bits, possibly none.
 */
uint32_t dmrIrq_takeEvents();

/**
 * Wake the thread waiting in dmrIrq_waitEvents, which then returns with
 * DMR_IRQ_EVENT_WAKE set, for instance to let it exit.
 */
void dmrIrq_wakeWaiter();

#ifdef __cplusplus
}
#endif

#endif /* DMRIRQ_H */
