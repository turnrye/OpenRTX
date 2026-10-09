/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_C6000_H
#define DMR_C6000_H

#include "interfaces/dmr.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Advance the transmit slot state machine by one slot. Called from the
 * timeslot interrupt handler of the radios transmitting from it while a
 * transmission is active, it must not be used from a thread as it drives the
 * chip without taking the SPI mutex.
 */
void dmr_txTimeslotIsr();

#ifdef __cplusplus
}
#endif

#endif /* DMR_C6000_H */
