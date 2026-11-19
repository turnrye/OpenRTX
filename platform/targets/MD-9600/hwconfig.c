/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "drivers/SPI/spi_custom.h"
#include "drivers/SPI/spi_bitbang.h"
#include "drivers/baseband/LMX2571.h"
#include "drivers/baseband/AK2365A.h"
#include "drivers/baseband/SKY72310.h"
#include "peripherals/gpio.h"
#include "hwconfig.h"
#include "drivers/SPI/spi_stm32.h"
#include "drivers/ADC/adc_stm32.h"
#include "drivers/GPS/gps_stm32.h"
#include <pthread.h>
#include "core/gps.h"

static pthread_mutex_t c6000_mutex;

/* Bench test: NOPs between the CLK falling edge and the MISO sample */
volatile uint32_t c6000ReadDelay = 16;

/**
 * SPI bitbang function for HR_C6000 command interface (U_SPI).
 *
 * Hand-tuned to be as fast as possible, gives the following clock performance
 * when compiled with -Os and run on STM32F405 at 168MHz:
 *
 * - Freq 6.46MHz
 * - Pos. width 71ns
 * - Neg. with 83ns
 */
static uint8_t spiC6000_func(const void *priv, uint8_t value)
{
    (void) priv;
    uint8_t incoming = 0;

    __disable_irq();

    for(uint8_t cnt = 0; cnt < 8; cnt++)
    {
        GPIOE->BSRR = (1 << 3);     // Set PE3 (CLK)

        if(value & (0x80 >> cnt))
            GPIOE->BSRR = 1 << 5;   // Set PE5 (MOSI)
        else
            GPIOE->BSRR = 1 << 21;  // Clear PE5 (MOSI)

        // ~70ns delay
        asm volatile("           mov   r1, #1     \n"
                     "___loop_1: cmp   r1, #0     \n"
                     "           itt   ne         \n"
                     "           subne r1, r1, #1 \n"
                     "           bne   ___loop_1  \n":::"r1");

        incoming <<= 1;
        GPIOE->BSRR = (1 << 19);                // Clear PE3 (CLK)

        // Bench test: MISO needs time to settle after the falling edge, at
        // full speed reads come back wrong (0xE0 = 0xC1 reads 0x41)
        for(uint32_t d = c6000ReadDelay; d > 0; d--)
            asm volatile("nop");

        incoming |= (GPIOE->IDR >> 4) & 0x01;   // Read PE4 (MISO)
    }

    __enable_irq();

    return incoming;
}

static void gpsEnable(void *priv)
{
    gpio_setPin(GPS_EN);
    gpsStm32_enable(priv);
}

static void gpsDisable(void *priv)
{
    gpio_clearPin(GPS_EN);
    gpsStm32_disable(priv);
}

static pthread_mutex_t spi2Mutex;
static pthread_mutex_t adcMutex;

static const struct spiConfig pllSpiCfg =
{
    .clk       = { PLL_CLK  },
    .mosi      = { PLL_DATA },
    .miso      = { PLL_DATA },
    .clkPeriod = SCK_PERIOD_FROM_FREQ(1000000),
    .flags     = SPI_HALF_DUPLEX
};

static const struct spiConfig ifSpiCfg =
{
    .clk       = { AK2365A_CLK  },
    .mosi      = { AK2365A_DATA },
    .miso      = { AK2365A_DATA },
    .clkPeriod = SCK_PERIOD_FROM_FREQ(1000000),
    .flags     = SPI_HALF_DUPLEX
};

SPI_CUSTOM_DEVICE_DEFINE(c6000_spi, spiC6000_func, NULL, &c6000_mutex)
SPI_STM32_DEVICE_DEFINE(spi2, SPI2, &spi2Mutex)
SPI_BITBANG_DEVICE_DEFINE(pll_spi, pllSpiCfg, NULL)
SPI_BITBANG_DEVICE_DEFINE(if_spi, ifSpiCfg, NULL)

/* IF chips of the V1-V4 boards, probed at boot */
const struct ak2365a ifVhf =
{
    .spi = (const struct spiDevice *) &if_spi,
    .cs  = { AK2365A_CS_VHF },
    .res = { AK2365A_RST }
};

const struct ak2365a ifUhf =
{
    .spi = (const struct spiDevice *) &if_spi,
    .cs  = { AK2365A_CS_UHF },
    .res = { AK2365A_RST }
};
ADC_STM32_DEVICE_DEFINE(adc1, ADC1, &adcMutex, ADC_COUNTS_TO_UV(3300000, 12))

/*
 * LMX2571 setup of the V5 board, as written by the stock S126.061 firmware
 * (literals at 0x0806f694-0x0806f784) and documented by RadioFoundry,
 * boards/md9600/rfplan_md9600.c, https://github.com/radiofoundry/radiofoundry:
 * 16.8 MHz reference, pre-divider 8 on VHF and 4 on UHF.
 */
static const struct lmx2571Config pllVhfCfg =
{
    .refClk    = 16800000,
    .r0        = 0x003E,
    .r6        = 0x8581,
    .r7        = 0x1024,
    .r8        = 0x0430,
    .r41       = 0x0230,
    .r42       = 0x0108,
    .refDiv    = 8,
    .fracOrder = 3
};

static const struct lmx2571Config pllUhfCfg =
{
    .refClk    = 16800000,
    .r0        = 0x003E,
    .r6        = 0x8581,
    .r7        = 0x1024,
    .r8        = 0x0430,
    .r41       = 0x0230,
    .r42       = 0x0108,
    .refDiv    = 4,
    .fracOrder = 3
};

const struct lmx2571 pllVhf =
{
    .spi = (const struct spiDevice *) &pll_spi,
    .cfg = &pllVhfCfg,
    .cs  = { PLL_CS_VHF }
};

const struct lmx2571 pllUhf =
{
    .spi = (const struct spiDevice *) &pll_spi,
    .cfg = &pllUhfCfg,
    .cs  = { PLL_CS_UHF }
};

/*
 * SKY72310 synthesizers of the V1-V3 boards, on the same bus and chip selects
 * as the V5's LMX2571: RadioFoundry, docs/spec/hardware-md9600.md section
 * 5.3, https://github.com/radiofoundry/radiofoundry
 */
const struct sky73210 skyVhf =
{
    .spi    = (const struct spiDevice *) &pll_spi,
    .cs     = { PLL_CS_VHF },
    .refClk = 16800000
};

const struct sky73210 skyUhf =
{
    .spi    = (const struct spiDevice *) &pll_spi,
    .cs     = { PLL_CS_UHF },
    .refClk = 16800000
};

const struct gpsDevice gps =
{
    .priv = NULL,
    .enable = gpsEnable,
    .disable = gpsDisable,
    .getSentence = gpsStm32_getNmeaSentence
};
