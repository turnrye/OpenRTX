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

/* Message inbox infrastructure, large snapshot tier. The M17 SMS source
 * pool tier below overrides the m17_sms.cpp default (32 entries / 3200
 * bytes). */
#define CONFIG_MESSAGES
#define CONFIG_MSG_SNAPSHOT_SIZE 64

/* M17 SMS message source for the generic inbox. */
#define CONFIG_M17_SMS
#define CONFIG_M17_SMS_MAX_MESSAGES 64
#define CONFIG_M17_SMS_POOL_BYTES 6400

#endif /* HWCONFIG_H */
