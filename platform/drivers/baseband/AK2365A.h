/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AK2365A_H
#define AK2365A_H

#include "peripherals/gpio.h"
#include "peripherals/spi.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * IF filter selection, value is BPF_BW[2:0]. BPF_BW[1:0] are bits 3-2 of
 * register 0x01, BPF_BW[2] is bit 7 of register 0x0B. Bandwidths are the
 * -6dB points around the 450kHz IF.
 */
enum AK2365A_BPF
{
    AK2365A_BPF_7p5 = 4,    ///< F0, BPF_BW = 1xx, +/-7.5kHz
    AK2365A_BPF_6   = 0,    ///< F1, BPF_BW = 000, +/-6kHz
    AK2365A_BPF_4p5 = 1,    ///< F2, BPF_BW = 001, +/-4.5kHz
    AK2365A_BPF_3   = 2,    ///< F3, BPF_BW = 010, +/-3kHz
    AK2365A_BPF_2   = 3,    ///< F4, BPF_BW = 011, +/-2kHz
};

/*
 * Register fields, from the AK2365A data sheet MS1591-E-03, section 15
 * "Serial Interface Configuration", pages 18-22,
 * https://datasheet.chipsmall.com/AK2365A/123777.pdf
 */
#define AK2365A_REG01_BS(x)        (((x) & 0x07) << 5)  ///< Operation mode
#define AK2365A_REG01_BAND         0x10                 ///< Wide demod. level
#define AK2365A_REG01_BPF_BW(x)    (((x) & 0x03) << 2)  ///< BPF_BW[1:0]
#define AK2365A_REG01_LOFREQ(x)    ((x) & 0x03)         ///< Second LO
#define AK2365A_REG02_AGC_KEEP     0x40                 ///< Hold AGC gains
#define AK2365A_REG02_AGC_FAST     0x20                 ///< Fixed AGC speed
#define AK2365A_REG02_RSVD4        0x10                 ///< Reserved, 1
#define AK2365A_REG02_AGC_TIME(x)  (((x) & 0x03) << 2)  ///< AGC response time
#define AK2365A_REG02_AGC1_STEP    0x02                 ///< AGC1 2 dB steps
#define AK2365A_REG02_CAL          0x01                 ///< Start calibration
#define AK2365A_REG03_IFOG(x)      ((x) & 0x03)         ///< IF buffer gain
#define AK2365A_REG04_SRST         0xAA                 ///< Software reset
#define AK2365A_REG0B_BPF_BW2(x)   (((x) & 0x04) << 5)  ///< BPF_BW[2]
#define AK2365A_REG0B_AGC_OFF      0x40                 ///< Manual AGC
#define AK2365A_REG0B_AGC1_G(x)    ((x) & 0x3F)         ///< Manual AGC1 gain
#define AK2365A_REG0C_RSVD7        0x80                 ///< Reserved, 1
#define AK2365A_REG0C_AGC2_G(x)    ((x) & 0x1F)         ///< Manual AGC2 gain

/**
 * AK2365A device data.
 */
struct ak2365a
{
    const struct spiDevice *spi;   ///< SPI bus device driver
    const struct gpioPin   cs;     ///< Chip select gpio
    const struct gpioPin   res;    ///< Reset gpio
};


/**
 * Initialise the FM detector IC.
 *
 * @param dev: pointer to device data.
 */
void AK2365A_init(const struct ak2365a *dev);

/**
 * Terminate the driver and set the IC in reset state.
 *
 * @param dev: pointer to device data.
 */
void AK2365A_terminate(const struct ak2365a *dev);

/**
 * Set the bandwidth of the internal IF filter.
 *
 * @param dev: pointer to device data.
 * @param bw: bandwidth.
 */
void AK2365A_setFilterBandwidth(const struct ak2365a *dev, const uint8_t bw);

/**
 * Reset the chip and set it up for receive: every stage on, 50.4MHz second
 * LO, automatic gain control at the data sheet's recommended speed, IF buffer
 * gain 6dB, then start the discriminator calibration. The calibration takes
 * 1.3ms and the discriminator another 1.5ms to settle (data sheet section
 * 16). The reset line may be shared: a chip on the same line is reset too.
 *
 * Register values from RadioFoundry, docs/spec/hardware-md9600.md section
 * 5.4, as used on the MD-9600 V1-V3,
 * https://github.com/radiofoundry/radiofoundry
 *
 * @param dev: pointer to device data.
 * @param bw: IF filter bandwidth.
 * @param wide: demodulated level for 3kHz deviation instead of 1.5kHz.
 */
void AK2365A_setupRx(const struct ak2365a *dev, const enum AK2365A_BPF bw,
                     const bool wide);

/**
 * Check whether the chip is fitted and answers. Two patterns are written to
 * a read-write register and read back, each with the data line pulled down
 * and then up while released; the chip is then reset by software. The reset
 * line is only pulsed through its pull resistors, never driven. Needs a
 * bitbang bus and the chip powered.
 *
 * Read test from RadioFoundry, boards/md9600/rfseq_md9600.c
 * md9600_rf_if_detect() and rev_md9600.c md9600_if_verdict(),
 * https://github.com/radiofoundry/radiofoundry
 *
 * @param dev: pointer to device data.
 * @return 0 if the chip read back both patterns, -ENODEV if every sample
 * followed the pull (nothing drives the line), -EIO otherwise.
 */
int AK2365A_probe(const struct ak2365a *dev);

#ifdef __cplusplus
}
#endif

#endif /* AK2365A_H */
