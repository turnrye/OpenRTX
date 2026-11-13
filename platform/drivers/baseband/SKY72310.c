/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/delays.h"
#include "drivers/baseband/SKY72310.h"

/*
 * Main divide ratio, fractional mode: the register holds the whole part less
 * N_OFFSET, the dividend the fraction in FRAC_BITS bits.
 */
enum {
    N_OFFSET = 32,
    FRAC_BITS = 18,
    FRAC_MASK = (1 << FRAC_BITS) - 1,
};

static inline void writeReg(const struct sky73210 *dev, const uint16_t value)
{
    const uint16_t tmp = __builtin_bswap16(value);

    /*
     * NOTE: for some (yet) unknown reason, there must be at least 10us between
     * chip select assertion/deassertion and the SPI transaction. For lower
     * values the PLL seems to go nuts.
     */
    spi_acquire(dev->spi);
    gpioPin_clear(&dev->cs);
    delayUs(10);

    spi_send(dev->spi, &tmp, 2);

    delayUs(10);
    gpioPin_set(&dev->cs);
    spi_release(dev->spi);
}

void SKY73210_init(const struct sky73210 *dev, const uint8_t gain)
{
    gpioPin_setMode(&dev->cs, OUTPUT);
    gpioPin_set(&dev->cs);

    writeReg(dev, 0x6000 | (gain & 0x1F)); // Phase detector gain
    writeReg(dev, 0x73D0); // Power down/multiplexer control register
    writeReg(dev, 0x8000); // Modulation control register
    writeReg(dev, 0x9000); // Modulation data register
}

void SKY73210_terminate(const struct sky73210 *dev)
{
    (void)dev;
}

void SKY73210_setFrequency(const struct sky73210 *dev, const uint32_t freq,
                           uint8_t clkDiv)
{
    // Maximum allowable value for reference clock divider is 32
    if (clkDiv > 32)
        clkDiv = 32;

    /*
     * Divide ratio in steps of 2^-18, rounded to the nearest step, then split
     * into the nearest whole number and a fraction of -0.5 to +0.5, sent as
     * an 18-bit two's complement dividend. Integer arithmetic, as in
     * RadioFoundry, drivers/rf/sky72310/sky72310.c sky72310_ratio(),
     * https://github.com/radiofoundry/radiofoundry, Copyright (c) 2026 The
     * RadioFoundry Authors, MIT licence: single precision floats were off by
     * up to three steps.
     */
    const uint64_t ratio = ((uint64_t)freq * clkDiv) << FRAC_BITS;
    const uint64_t steps = (ratio + (dev->refClk / 2)) / dev->refClk;
    const uint64_t whole = (steps + (1 << (FRAC_BITS - 1))) >> FRAC_BITS;
    const uint32_t dnd = (steps - (whole << FRAC_BITS)) & FRAC_MASK;
    const uint16_t Ndiv = whole - N_OFFSET;

    uint16_t dndMsb = dnd >> 8;
    uint16_t dndLsb = dnd & 0x00FF;

    writeReg(dev, Ndiv);                            // Divider register
    writeReg(dev, 0x2000 | dndLsb);                 // Dividend LSB register
    writeReg(dev, 0x1000 | dndMsb);                 // Dividend MSB register
    writeReg(dev, 0x5000 | ((uint16_t)clkDiv - 1)); // Reference clock divider
}
