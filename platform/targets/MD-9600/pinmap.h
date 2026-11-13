/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PINMAP_H
#define PINMAP_H

#include "stm32f4xx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Power keep switch */
#define PWR_SW GPIOD,15

/* Display */
#define LCD_BKLIGHT GPIOC,6
#define LCD_RST     GPIOD,12
#define LCD_RS      GPIOD,13
#define LCD_CS      GPIOD,14

/* Analog inputs */
#define AIN_RSSI_VHF  GPIOA,0
#define AIN_VBAT      GPIOA,1
#define AIN_MIC       GPIOA,3
#define AIN_SW2       GPIOA,6
#define AIN_SW1       GPIOA,7
#define AIN_RSSI_UHF  GPIOB,0
#define AIN_NOISE_UHF GPIOB,1
#define AIN_NOISE_VHF GPIOC,5

/*
 * Channel selection rotary encoder, in the order that makes the position
 * count up when turned clockwise, as on the MD-UV3x0
 */
#define CH_SELECTOR_0 GPIOB,11
#define CH_SELECTOR_1 GPIOB,10

/* Push-to-talk switch */
#define PTT_SW GPIOE,10

/* Keyboard */
#define KB_ROW1 GPIOD,2
#define KB_ROW2 GPIOD,3
#define KB_ROW3 GPIOD,4
#define KB_COL1 GPIOD,0
#define KB_COL2 GPIOD,1
#define KB_COL3 GPIOE,0
#define KB_COL4 GPIOE,1

/* Tone generator  */
#define CTCSS_OUT GPIOC,7   /* CTCSS tone    */
#define BEEP_OUT  GPIOC,8   /* System "beep" */

/* SPI2, connected to external flash and LCD */
#define FLASH_CS &GpioB,12
#define SPI2_CLK GPIOB,13
#define SPI2_SDO GPIOB,14
#define SPI2_SDI GPIOB,15

/* Audio control */
#define SPK_MUTE GPIOB,6

/* GPS, for the devices who have it */
#define GPS_EN   GPIOA,9
#define GPS_DATA GPIOA,10

/* HR_C6000 control interface */
#define DMR_CS    &GpioE,2
#define DMR_CLK   GPIOE,3
#define DMR_MOSI  GPIOE,5
#define DMR_MISO  GPIOE,4
#define DMR_SLEEP GPIOE,6

/*
 * RF section of the V5 board (LMX2571 synthesizers, no IF chip). Pin roles
 * from RadioFoundry, boards/md9600/pins.h and docs/spec/hardware-md9600.md
 * section 2, checked against the stock S126.061 firmware,
 * https://github.com/radiofoundry/radiofoundry
 */

/* Supplies and VCO selection */
#define RF_C5_UHF   GPIOA,8    /* UHF 5 V, receiver side              */
#define RF_C5_VHF   GPIOC,9    /* VHF 5 V, receiver side              */
#define RF_R5_UHF   GPIOA,13   /* UHF receiver 5 V, also SWDIO        */
#define RF_R5_VHF   GPIOA,14   /* VHF receiver 5 V, also SWCLK        */
#define RF_T5_VHF   GPIOD,5    /* VHF transmitter 5 V                 */
#define RF_T5_UHF   GPIOD,6    /* UHF transmitter 5 V                 */
#define RF_APC_EN   GPIOC,4    /* PA power loop                       */
#define VCO_SEL_UHF GPIOE,8    /* High = RX VCO, low = TX VCO         */
#define VCO_SEL_VHF GPIOE,9    /* High = RX VCO, low = TX VCO         */
#define MOD_SW      GPIOB,7    /* MOD2 to the VHF (high) or UHF VCO   */

/* LMX2571 synthesizers */
#define PLL_CS_VHF  &GpioD,8
#define PLL_CS_UHF  &GpioD,9
#define PLL_DATA    GPIOD,10
#define PLL_CLK     GPIOD,11

/* IF filter bandwidth (high = 25 kHz) and route to the HR_C6000 */
#define IF_BW_VHF    GPIOE,11
#define IF_BW_UHF    GPIOE,12
#define IF_ROUTE_VHF GPIOE,13
#define IF_ROUTE_UHF GPIOE,14

/*
 * AK2365A IF chips of the V1-V4 boards, on the same pins as the V5 IF lines:
 * probed at boot to tell the boards apart
 */
#define AK2365A_CS_VHF &GpioE,11
#define AK2365A_CS_UHF &GpioE,12
#define AK2365A_DATA   GPIOE,13
#define AK2365A_CLK    GPIOE,14
#define AK2365A_RST    &GpioE,15

/* Front-end tuning */
#define TUNE_UHF GPIOA,4       /* DAC1                                */
#define TUNE_VHF GPIOA,5       /* DAC2, PA power reference in TX      */

/* Receive audio */
#define FM_SW   GPIOB,2        /* FM audio stages on                  */
#define AF_BAND GPIOC,13       /* Audio from VHF (high) or UHF (low)  */
#define FM_MUTE GPIOE,7        /* High = FM audio to the volume knob  */

#ifdef __cplusplus
}
#endif

#endif /* PINMAP_H */
