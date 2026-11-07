/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include "hwconfig.h"
#include <stdbool.h>

typedef enum
{
    TIMER_OFF =  0,
    TIMER_5S  =  1,
    TIMER_10S =  2,
    TIMER_15S =  3,
    TIMER_20S =  4,
    TIMER_25S =  5,
    TIMER_30S =  6,
    TIMER_1M  =  7,
    TIMER_2M  =  8,
    TIMER_3M  =  9,
    TIMER_4M  = 10,
    TIMER_5M  = 11,
    TIMER_15M = 12,
    TIMER_30M = 13,
    TIMER_45M = 14,
    TIMER_1H  = 15
}
display_timer_t;

typedef struct
{
    uint8_t brightness;           // Display brightness
    uint8_t contrast;             // Display contrast
    uint8_t sqlLevel;             // Squelch level
    uint8_t voxLevel;             // Vox level
    int8_t  utc_timezone;         // Timezone, in units of half hours
    bool    gps_enabled;          // GPS active
    char    callsign[10];         // Plaintext callsign
    uint8_t display_timer   : 4,  // Standby timer
            m17_can         : 4;  // M17 CAN
    uint8_t vpLevel         : 3,  // Voice prompt level
            vpPhoneticSpell : 1,  // Phonetic spell enabled
            macroMenuLatch  : 1,  // Automatic latch of macro menu
            _reserved       : 3;
    bool    m17_can_rx;           // Check M17 CAN on RX
    char    m17_dest[10];         // M17 destination
    bool    showBatteryIcon;      // Battery display true: icon, false: percentage
    bool    gpsSetTime;           // Use GPS to ajust RTC time
    char    M17_meta_text[53];    // M17 Meta Text to send
    char    aprs_callsign[10];    // APRS station address, "CALL" or "CALL-SSID"
    uint8_t beacon_method   : 2,  // Beacon: off/manual/interval/adaptive
            beacon_pos_fixed: 1,  // Beacon uses the fixed position, not GPS
            beacon_send_cs  : 1,  // Include course/speed in beacons
            beacon_send_alt : 1,  // Include altitude in beacons
            aprs_beacon     : 1,  // Beacon over APRS
            m17_beacon      : 1,  // Carry the position in M17 metadata
            _reserved_aprs  : 1;
    uint16_t beacon_interval;     // Beacon interval / rate floor, seconds
    uint16_t beacon_maxInterval;  // Beacon heartbeat ceiling, seconds
    uint16_t beacon_errThreshold; // Adaptive beacon error threshold, metres
    char    aprs_symTable;        // APRS symbol table id, e.g. '/'
    char    aprs_symCode;         // APRS symbol code, e.g. '['
    char    aprs_path[20];        // APRS digipeater path
    char    beacon_comment[32];   // Beacon comment text
    int32_t beacon_fixedLat;      // Fixed-position latitude, microdegrees
    int32_t beacon_fixedLon;      // Fixed-position longitude, microdegrees
}
__attribute__((packed)) settings_t;


static const settings_t default_settings =
{
    100,                          // Brightness
#ifdef CONFIG_SCREEN_CONTRAST
    CONFIG_DEFAULT_CONTRAST,      // Contrast
#else
    255,                          // Contrast
#endif
    4,                            // Squelch level, 4 = S3
    0,                            // Vox level
    0,                            // UTC Timezone
    false,                        // GPS enabled
    "N0CALL",                     // Default callsign
    TIMER_30S,                    // 30 seconds
    0,                            // M17 CAN
    0,                            // Voice prompts off
    0,                            // Phonetic spell off
    1,                            // Automatic latch of macro menu enabled
    0,                            // not used
    false,                        // Check M17 CAN on RX
    "",                           // Empty M17 destination
    false,                        // Display battery icon
    false,                        // Update RTC with GPS
    "OpenRTX",                    // Default M17 meta text
    "N0CALL",                     // Default APRS callsign
    0,                            // Beacon method: off
    false,                        // Beacon position source: GPS
    true,                         // Include course/speed
    true,                         // Include altitude
    true,                         // Beacon over APRS
    true,                         // Position in M17 metadata
    0,                            // not used
    600,                          // Beacon interval, seconds
    1800,                         // Beacon heartbeat, seconds
    600,                          // Adaptive beacon error threshold, metres
    '/',                          // APRS symbol table
    '[',                          // APRS symbol code (person)
    "WIDE1-1,WIDE2-1",            // APRS digipeater path
    "OpenRTX",                    // Beacon comment
    0,                            // APRS fixed latitude, microdegrees
    0,                            // APRS fixed longitude, microdegrees
};

#endif /* SETTINGS_H */
