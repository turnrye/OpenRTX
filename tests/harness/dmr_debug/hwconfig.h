/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HWCONFIG_H
#define HWCONFIG_H

/*
 * The DMR debug tap test's radio: DMR enabled, the tap carried over USB serial
 * so that both its output and its commands go through functions the test
 * fakes.
 */
#define CONFIG_DMR
#define CONFIG_USB_SERIAL

#endif
