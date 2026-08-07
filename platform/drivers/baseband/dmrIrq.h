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
 * The chip registers describing a received burst which are only meaningful at
 * the instant of the frame event: read from the consuming thread they arrive
 * too late and describe nothing. The system interrupt handler samples them
 * together so that the values are consistent with each other and with the
 * burst which raised the event.
 */
struct dmrIrqRxSnapshot
{
    uint8_t intFlags;   ///< Interrupt flags, register 0x82.
    uint8_t status;     ///< Data type, sync class and CRC result, register 0x51.
    uint8_t cach;       ///< Colour code and timecode, register 0x52.
    uint8_t sync;       ///< Detected sync pattern, register 0x5F.
    bool    valid;      ///< False until a frame event has been sampled.
};

/**
 * Sync pattern the chip detected, held in the low two bits of register 0x5F.
 * A base station sync means the far end is a repeater; the two direct mode
 * values name the timeslot the transmission calls itself, which is the only
 * place a timeslot number appears on air in direct mode.
 */
enum dmrRxSyncType
{
    DMR_SYNC_MS   = 0,  ///< Mobile station sourced.
    DMR_SYNC_BS   = 1,  ///< Base station sourced.
    DMR_SYNC_TS1  = 2,  ///< Direct mode, timeslot 1.
    DMR_SYNC_TS2  = 3   ///< Direct mode, timeslot 2.
};

/**
 * Get the registers sampled at the most recent frame event.
 *
 * @param snapshot: destination for the sampled values.
 */
void dmrIrq_getRxSnapshot(struct dmrIrqRxSnapshot *snapshot);

/**
 * Get the number of frame events whose registers could not be sampled because
 * a thread held the SPI bus, or because the radio was transmitting. Those
 * events leave the snapshot holding the values of an earlier burst, so a
 * reading of it is only worth as much as this counter is small.
 *
 * @return the number of frame events which went unsampled.
 */
uint32_t dmrIrq_getRxUnsampled();

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

#ifdef __cplusplus
}
#endif

#endif /* DMRIRQ_H */
