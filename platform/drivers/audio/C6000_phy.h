/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef C6000_PHY_H
#define C6000_PHY_H

#include <stddef.h>
#include "drivers/baseband/HR_C6000.h"

/**
 * TEST ONLY: the HR_C6000's physical layer receive, driven from the bench
 * console. The chip slices 144 symbols per 30 ms record into RAM page 2 at
 * 0x30 and flags each record in register 0x82 bit 0; this module polls the
 * flag, reads the record and the registers the host asks for, and sends them
 * to the console as PHR and PHD lines. The host sets the chip up and pokes it
 * through the console commands of C6000phy_command().
 */

/**
 * Give the module the HR_C6000 it drives.
 *
 * @param chip: the radio's HR_C6000.
 */
void C6000phy_init(HR_C6000 *chip);

/**
 * Count a timeslot interrupt, from the ISR.
 */
void C6000phy_timeslotIrq();

/**
 * Poll the chip and send the records; C6000phy_init() starts a thread that
 * calls it every 2 ms.
 */
void C6000phy_task();

/**
 * Whether the physical layer receive owns the chip.
 *
 * @return true while it is on.
 */
bool C6000phy_active();

/**
 * Run one PHY console command.
 *
 * @param args: the command's arguments, after "PHY ".
 * @param reply: the reply, after "PHY <ms> ".
 * @param len: size of reply.
 * @return zero on success, a negative value for a bad command.
 */
extern "C" int C6000phy_command(char *args, char *reply, size_t len);

#endif /* C6000_PHY_H */
