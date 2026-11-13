/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HWCONFIG_H
#define HWCONFIG_H

#include "stm32f4xx.h"
#include "pinmap.h"

#ifdef __cplusplus

// Export the HR_C6000 driver only for C++ sources
#include "drivers/baseband/HR_C6000.h"

extern HR_C6000 C6000;

extern "C" {
#endif

enum adcChannel {
    ADC_RSSI_VHF_CH = 0,
    ADC_VBAT_CH = 1,
    ADC_VOX_CH = 3,
    ADC_RSSI_UHF_CH = 8,
    ADC_SW1_CH = 7,
    ADC_SW2_CH = 6,
    ADC_NOISE_UHF_CH = 9,
    ADC_NOISE_VHF_CH = 15,
    ADC_CTCSS_CH = 2,
};

extern const struct gpsDevice gps;
extern const struct spiCustomDevice c6000_spi;
extern const struct spiCustomDevice pll_spi;
extern const struct spiDevice spi2;
extern const struct lmx2571 pllVhf;
extern const struct lmx2571 pllUhf;
extern const struct spiCustomDevice if_spi;
extern const struct ak2365a ifVhf;
extern const struct ak2365a ifUhf;
extern const struct Adc adc1;

/* Board revision, in hwInfo.flags */
enum md9600Flags {
    MD9600_FLAGS_V5 = 1,    ///< V4A/V5: LMX2571 synthesizers, no IF chip
    MD9600_FLAGS_V1_V4 = 2, ///< V1-V4: AK2365A IF chips
};

/* Device has a working real time clock */
#define CONFIG_RTC

/* Device supports an optional GPS chip */
#define CONFIG_GPS
#define CONFIG_GPS_STM32_USART1
#define CONFIG_NMEA_RBUF_SIZE 128

/* Screen dimensions */
#define CONFIG_SCREEN_WIDTH 128
#define CONFIG_SCREEN_HEIGHT 64

/* Screen has adjustable contrast */
#define CONFIG_SCREEN_CONTRAST
#define CONFIG_DEFAULT_CONTRAST 91

/* Screen has adjustable brightness */
#define CONFIG_SCREEN_BRIGHTNESS

/* Screen pixel format */
#define CONFIG_PIX_FMT_BW

/* Battery type */
#define CONFIG_BAT_NONE

/* Restart into the TYT bootloader from the settings menu */
#define CONFIG_BOOTLOADER_ENTRY

/* Device supports M17 mode */
#define CONFIG_M17

/*
 * Transmit power levels, in mW: the factory labels of the four PA drive rows
 * in the calibration, lowest first
 */
#define CONFIG_TX_POWER_LEVELS 5000, 10000, 25000, 40000

/* Microphone audio input */
#define CONFIG_MIC_GAIN 32
#define CONFIG_MIC_OVERSAMPLE 8

/*
 * The speaker amplifier takes about 60 ms from unmute to sound. Value from
 * RadioFoundry, boards/md9600/c6000_md9600.c speaker_lead_ms,
 * https://github.com/radiofoundry/radiofoundry
 */
#define CONFIG_SPK_STARTUP_MS 80

#ifdef __cplusplus
}
#endif

#endif /* HWCONFIG_H */
