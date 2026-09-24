/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "drivers/SPI/spi_stm32.h"
#include "peripherals/gpio.h"
#include "peripherals/spi.h"
#include "hwconfig.h"
#include "dmrPayload.h"
#include <string.h>

#if defined(CONFIG_DMR)

/*
 * The CS7000 Plus voice payload bus: the HR_C6000's V_SPI on SPI1, with the
 * clock on PB3, the data lines on PB4 and PB5 and a software chip select on
 * PB6. The read command is the page number with the read flag set, followed by
 * the register offset; the payload comes back after those two command bytes,
 * so the transfer is two bytes longer than the data.
 */
#define VP_PAGE_READ            0x83
#define VP_PAGE_WRITE           0x03
#define VP_REGISTER             0x00

/**
 * Bring up the voice payload bus. PB3 and PB4 come out of reset as JTAG pins,
 * so all three are explicitly given to SPI1. The chip wants the clock idle high
 * and data sampled on the falling edge, as on the MD-UV3x0.
 */
void dmrPayload_init()
{
    gpio_setMode(VOC_CLK,  ALTERNATE | ALTERNATE_FUNC(5));
    gpio_setMode(VOC_MISO, ALTERNATE | ALTERNATE_FUNC(5));
    gpio_setMode(VOC_MOSI, ALTERNATE | ALTERNATE_FUNC(5));
    gpio_setMode(VOC_CS,   OUTPUT);
    gpio_setPin(VOC_CS);        // Chip select idle high

    spiStm32_init(&voc_spi, 6000000, SPI_FLAG_CPOL);
}

void dmrPayload_terminate()
{
    spiStm32_terminate(&voc_spi);

    gpio_setMode(VOC_CLK,  INPUT);
    gpio_setMode(VOC_MISO, INPUT);
    gpio_setMode(VOC_MOSI, INPUT);
    gpio_setMode(VOC_CS,   INPUT);
}

void dmrPayload_read(uint8_t *payload)
{
    uint8_t tx[2 + DMR_PAYLOAD_LENGTH];
    uint8_t rx[2 + DMR_PAYLOAD_LENGTH];

    memset(tx, 0, sizeof(tx));
    tx[0] = VP_PAGE_READ;
    tx[1] = VP_REGISTER;

    gpio_clearPin(VOC_CS);
    spi_transfer(&voc_spi, tx, rx, sizeof(tx));
    gpio_setPin(VOC_CS);

    memcpy(payload, &rx[2], DMR_PAYLOAD_LENGTH);
}

void dmrPayload_write(const uint8_t *payload)
{
    uint8_t tx[2 + DMR_PAYLOAD_LENGTH];

    tx[0] = VP_PAGE_WRITE;
    tx[1] = VP_REGISTER;
    memcpy(&tx[2], payload, DMR_PAYLOAD_LENGTH);

    gpio_clearPin(VOC_CS);
    spi_transfer(&voc_spi, tx, NULL, sizeof(tx));
    gpio_setPin(VOC_CS);
}

#endif /* CONFIG_DMR */
