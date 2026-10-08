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
#define PLATFORM_CS7000P

#define DMR_TS_INT 0, 0
#define DMR_SYS_INT 0, 3
#define DMR_TX_INT 0, 2

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
uint8_t gpio_readPin(const void *port, const uint8_t pin);
#ifdef __cplusplus
}
#include "drivers/baseband/HR_C6000.h"
extern HR_C6000 C6000;
#endif

#endif
