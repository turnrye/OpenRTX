/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HWCONFIG_H
#define HWCONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Module17 hardware info flags, required by Module17 UI emulator target */
enum Mod17Flags {
    MOD17_FLAGS_HMI_PRESENT = 1,
    MOD17_FLAGS_SOFTPOT = 2,
};

/* Screen has adjustable brightness */
#define CONFIG_SCREEN_BRIGHTNESS

/* Battery type */
#define CONFIG_BAT_LIPO
#define CONFIG_BAT_NCELLS 2

/* Device supports M17 mode */
#define CONFIG_M17

#ifdef __cplusplus
}
#endif

/* Message inbox and M17 SMS. */
#define CONFIG_MESSAGES
#ifndef CONFIG_MESSAGES_MAX_ENTRIES
#define CONFIG_MESSAGES_MAX_ENTRIES 64
#endif
#ifndef CONFIG_MESSAGES_POOL_BYTES
#define CONFIG_MESSAGES_POOL_BYTES 6400
#endif
#define CONFIG_M17_SMS

/* APRS: AFSK1200 receive and transmit, with addressed messages in the
 * inbox. */
#define CONFIG_APRS

#endif /* HWCONFIG_H */
