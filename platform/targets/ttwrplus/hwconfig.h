/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HWCONFIG_H
#define HWCONFIG_H

#include <zephyr/device.h>

/*
 * Display properties are encoded in the devicetree
 */
#define DISPLAY DT_CHOSEN(zephyr_display)
#define CONFIG_SCREEN_WIDTH DT_PROP(DISPLAY, width)
#define CONFIG_SCREEN_HEIGHT DT_PROP(DISPLAY, height)
#define CONFIG_PIX_FMT_BW

#define CONFIG_GPS
#define CONFIG_NMEA_RBUF_SIZE 128

#define CONFIG_BAT_LIPO
#define CONFIG_BAT_NCELLS 1

#define CONFIG_M17

/* M17 SMS inbox capacity. */
#define M17_SMS_MAX_MESSAGES 64
#define M17_SMS_POOL_BYTES 4096
#define CONFIG_MSG_SNAPSHOT_SIZE (M17_SMS_MAX_MESSAGES)

#endif /* HWCONFIG_H */
