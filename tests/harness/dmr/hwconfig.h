/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HWCONFIG_H
#define HWCONFIG_H

/*
 * Hardware configuration of the DMR driver harness: the HR_C6000 DMR driver
 * built as for the CS7000 Plus, against a chip model in place of the chip.
 */

#define CONFIG_DMR

/* The CS7000 Plus unless the build asks for the MD-UV3x0. */
#if !defined(PLATFORM_MDUV3x0)
#define PLATFORM_CS7000P
#endif

#define DMR_TS_INT 0, 0
#define DMR_SYS_INT 0, 3
#define DMR_TX_INT 0, 2
#define DMR_SLEEP 0, 6

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
uint8_t gpio_readPin(const void *port, const uint8_t pin);
void gpio_setMode(const void *port, const uint8_t pin, const uint16_t mode);
void gpio_setPin(const void *port, const uint8_t pin);
void gpio_clearPin(const void *port, const uint8_t pin);
#ifdef __cplusplus
}
#include "drivers/baseband/HR_C6000.h"
extern HR_C6000 C6000;
#endif

#endif
