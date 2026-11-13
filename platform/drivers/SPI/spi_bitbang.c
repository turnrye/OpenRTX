/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/delays.h"
#include "spi_bitbang.h"

uint8_t spiBitbang_sendRecv(const void *priv, uint8_t data)
{
    const struct spiConfig *cfg = (const struct spiConfig *) priv;
    uint8_t incoming = 0;

    for(uint8_t cnt = 0; cnt < 8; cnt++)
    {
        // Clock inactive edge
        if((cfg->flags & SPI_FLAG_CPHA) != 0)
            gpio_setPin(cfg->clk.port, cfg->clk.pin);
        else
            gpio_clearPin(cfg->clk.port, cfg->clk.pin);

        // Setup data output
        if((data & 0x80) != 0)
            gpio_setPin(cfg->mosi.port, cfg->mosi.pin);
        else
            gpio_clearPin(cfg->mosi.port, cfg->mosi.pin);

        // Sample data input
        data     <<= 1;
        incoming <<= 1;
        incoming |= gpio_readPin(cfg->miso.port, cfg->miso.pin);

        delayUs(cfg->clkPeriod);

        // Clock active edge
        if((cfg->flags & SPI_FLAG_CPHA) != 0)
            gpio_clearPin(cfg->clk.port, cfg->clk.pin);
        else
            gpio_setPin(cfg->clk.port, cfg->clk.pin);

        delayUs(cfg->clkPeriod);
    }

    return incoming;
}


int spiBitbang_init(const struct spiCustomDevice *dev)
{
    const struct spiConfig *cfg = (const struct spiConfig *) dev->priv;

    // Basic driver init
    spi_init((const struct spiDevice *) dev);

    // Setup MOSI and clock lines as output, low level
    gpio_setMode(cfg->clk.port,   cfg->clk.pin,  OUTPUT);
    gpio_setMode(cfg->mosi.port,  cfg->mosi.pin, OUTPUT);
    gpio_clearPin(cfg->mosi.port, cfg->mosi.pin);
    gpio_clearPin(cfg->clk.port,  cfg->clk.pin);

    // Set MISO line as input only for full-duplex operation
    if((cfg->flags & SPI_HALF_DUPLEX) == 0)
        gpio_setMode(cfg->miso.port, cfg->miso.pin, INPUT);

    return 0;
}

uint32_t spiBitbang_read3wire(const struct spiCustomDevice *dev,
                              const struct gpioPin *cs, const uint32_t header,
                              const uint8_t headerBits, const uint16_t pull,
                              const uint32_t settleUs, const uint8_t bits)
{
    const struct spiConfig *cfg = (const struct spiConfig *) dev->priv;
    uint32_t value = 0;

    gpioPin_clear(cs);
    delayUs(cfg->clkPeriod);

    // Header, latched by the device on the rising edges
    for(uint32_t mask = 1UL << (headerBits - 1); mask != 0; mask >>= 1)
    {
        gpio_clearPin(cfg->clk.port, cfg->clk.pin);

        if((header & mask) != 0)
            gpio_setPin(cfg->mosi.port, cfg->mosi.pin);
        else
            gpio_clearPin(cfg->mosi.port, cfg->mosi.pin);

        delayUs(cfg->clkPeriod);
        gpio_setPin(cfg->clk.port, cfg->clk.pin);
        delayUs(cfg->clkPeriod);
    }

    // Turn the line around with the clock high, then sample after each
    // falling edge
    gpio_setMode(cfg->mosi.port, cfg->mosi.pin, pull);
    delayUs(settleUs);

    for(uint8_t i = 0; i < bits; i++)
    {
        gpio_clearPin(cfg->clk.port, cfg->clk.pin);
        delayUs(cfg->clkPeriod);
        value = (value << 1) | gpio_readPin(cfg->mosi.port, cfg->mosi.pin);

        if((i + 1) < bits)
        {
            gpio_setPin(cfg->clk.port, cfg->clk.pin);
            delayUs(cfg->clkPeriod);
        }
    }

    // Release the device before driving the data line again
    delayUs(cfg->clkPeriod);
    gpioPin_set(cs);
    gpio_setMode(cfg->mosi.port, cfg->mosi.pin, OUTPUT);
    gpio_clearPin(cfg->mosi.port, cfg->mosi.pin);

    return value;
}

void spiBitbang_terminate(const struct spiCustomDevice* dev)
{
    const struct spiConfig *cfg = (const struct spiConfig *) dev->priv;

    // Set clock and MOSI back to Hi-Z state
    gpio_setMode(cfg->clk.port,  cfg->clk.pin,  INPUT);
    gpio_setMode(cfg->mosi.port, cfg->mosi.pin, INPUT);

    // Final driver cleanup
    spi_terminate((const struct spiDevice *) dev);
}
