/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "drivers/baseband/LMX2571.h"

/*
 * Register map from the TI LMX2571 data sheet (SNAS654A), section 7.6,
 * https://www.ti.com/lit/ds/symlink/lmx2571.pdf. Only the F1 register set is
 * used.
 */
enum lmx2571Reg {
    LMX2571_REG00 = 0x00, ///< Reset, power down, loop calibration.
    LMX2571_REG01 = 0x01, ///< PLL_DEN[23:16] and PLL_NUM[23:16].
    LMX2571_REG02 = 0x02, ///< PLL_NUM[15:0].
    LMX2571_REG03 = 0x03, ///< PLL_DEN[15:0].
    LMX2571_REG04 = 0x04, ///< Fraction order and PLL_N.
    LMX2571_REG05 = 0x05, ///< PLL_R and PLL_R_PRE.
    LMX2571_REG06 = 0x06, ///< Loop filter, channel dividers, multiplier.
    LMX2571_REG07 = 0x07, ///< Output buffers, loop filter.
    LMX2571_REG08 = 0x08, ///< FSK mode, external VCO selection.
    LMX2571_REG41 = 0x29, ///< External VCO charge pump.
    LMX2571_REG42 = 0x2A, ///< External VCO charge pump.
};

#define LMX2571_REG00_RESET 0x2000
#define LMX2571_REG00_FCAL_EN 0x0001
#define LMX2571_REG01_PLL_NUM_HI(x) (((x) >> 16) & 0x00FF)
#define LMX2571_REG02_PLL_NUM_LO(x) ((x) & 0xFFFF)
#define LMX2571_REG04_FRAC_ORDER(x) (((x) & 0x07) << 12)
#define LMX2571_REG04_PLL_N(x) ((x) & 0x0FFF)
#define LMX2571_REG05_PLL_R(x) (((x) & 0xFF) << 8)
#define LMX2571_REG05_PLL_R_PRE(x) ((x) & 0xFF)

#define LMX2571_FRAC_BITS 24

/**
 * Write a register. Words are 24 bit, MSB first: the R/W bit (0 = write), a
 * 7-bit address and 16 data bits, latched when the chip select rises.
 *
 * @param dev: pointer to device descriptor.
 * @param reg: register address.
 * @param value: register value.
 */
static void writeReg(const struct lmx2571 *dev, const enum lmx2571Reg reg,
                     const uint16_t value)
{
    const uint8_t data[] = {
        reg & 0x7F,
        value >> 8,
        value & 0xFF,
    };

    gpioPin_clear(&dev->cs);
    spi_send(dev->spi, data, sizeof(data));
    gpioPin_set(&dev->cs);
}

void LMX2571_init(const struct lmx2571 *dev)
{
    gpioPin_setMode(&dev->cs, OUTPUT);
    gpioPin_set(&dev->cs);

    writeReg(dev, LMX2571_REG00, LMX2571_REG00_RESET);
}

void LMX2571_setFrequency(const struct lmx2571 *dev, const uint32_t freq)
{
    const struct lmx2571Config *cfg = dev->cfg;

    // Division ratio to the phase detector, with a 24-bit fraction, rounded
    uint64_t ratio = ((uint64_t)freq * cfg->refDiv) << LMX2571_FRAC_BITS;
    ratio = (ratio + (cfg->refClk / 2)) / cfg->refClk;

    const uint32_t n = ratio >> LMX2571_FRAC_BITS;
    const uint32_t frac = ratio & ((1UL << LMX2571_FRAC_BITS) - 1);

    writeReg(dev, LMX2571_REG00, cfg->r0);
    writeReg(dev, LMX2571_REG04,
             LMX2571_REG04_FRAC_ORDER(cfg->fracOrder) | LMX2571_REG04_PLL_N(n));
    writeReg(dev, LMX2571_REG01, LMX2571_REG01_PLL_NUM_HI(frac));
    writeReg(dev, LMX2571_REG02, LMX2571_REG02_PLL_NUM_LO(frac));
    writeReg(dev, LMX2571_REG03, 0);
    writeReg(dev, LMX2571_REG05,
             LMX2571_REG05_PLL_R(1) | LMX2571_REG05_PLL_R_PRE(cfg->refDiv));
    writeReg(dev, LMX2571_REG06, cfg->r6);
    writeReg(dev, LMX2571_REG07, cfg->r7);
    writeReg(dev, LMX2571_REG08, cfg->r8);
    writeReg(dev, LMX2571_REG41, cfg->r41);
    writeReg(dev, LMX2571_REG42, cfg->r42);

    // A new N divider needs the loop calibrated (data sheet section 7.5.2)
    writeReg(dev, LMX2571_REG00, cfg->r0 | LMX2571_REG00_FCAL_EN);
    writeReg(dev, LMX2571_REG00, cfg->r0);
}
