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
#include "interfaces/delays.h"
#include <string.h>

#if defined(CONFIG_DMR)

/*
 * The CS7000 Plus voice payload bus: the HR_C6000's V_SPI on SPI1, with the
 * clock on PB3, the data lines on PB4 and PB5 and a software chip select on
 * PB6. It carries the payload to transmit; the write command is the page
 * number followed by the register offset. Received payloads do not come back
 * on it in this configuration - reading it returns zeros - so, as in the stock
 * firmware, they are read from the data page of the control interface.
 */
#define VP_PAGE_WRITE 0x03
#define VP_REGISTER 0x00

/*
 * Time the chip select is held low before the first clock edge and after the
 * last one, in microseconds. The chip needs it: with the clock starting right
 * after the select, as the hardware SPI does on its own, it ignored a share
 * of the transfers that drifted between a tenth and a third from one session
 * to the next, each ignored payload going out on air as a repeat of the
 * previous burst. The stock firmware gets the same margin from the code it
 * runs between the two steps.
 */
#define CS_SETUP_HOLD_US 10

/**
 * Bring up the voice payload bus. PB3 and PB4 come out of reset as JTAG pins,
 * so all three are explicitly given to SPI1. The bus runs as the stock firmware
 * drives it: SPI mode 1 (clock idle low, data sampled on the falling edge) at
 * well under 1MHz, which still moves a burst's payload in a fraction of a
 * slot.
 */
void dmrPayload_init()
{
    gpio_setMode(VOC_CLK, ALTERNATE | ALTERNATE_FUNC(5));
    gpio_setMode(VOC_MISO, ALTERNATE | ALTERNATE_FUNC(5));
    gpio_setMode(VOC_MOSI, ALTERNATE | ALTERNATE_FUNC(5));
    gpio_setMode(VOC_CS, OUTPUT);
    gpio_setPin(VOC_CS); // Chip select idle high

    spiStm32_init(&voc_spi, 1000000, SPI_FLAG_CPHA);
}

void dmrPayload_terminate()
{
    spiStm32_terminate(&voc_spi);

    gpio_setMode(VOC_CLK, INPUT);
    gpio_setMode(VOC_MISO, INPUT);
    gpio_setMode(VOC_MOSI, INPUT);
    gpio_setMode(VOC_CS, INPUT);
}

void dmrPayload_read(uint8_t *payload)
{
    C6000.readDmrVoicePayload(payload);
}

void dmrPayload_write(const uint8_t *payload)
{
    uint8_t tx[2 + DMR_PAYLOAD_LENGTH];

    tx[0] = VP_PAGE_WRITE;
    tx[1] = VP_REGISTER;
    memcpy(&tx[2], payload, DMR_PAYLOAD_LENGTH);

    /*
     * The transfer is run as the stock firmware runs it, rather than through
     * the generic driver: a transfer of known length, each byte shifted in
     * full before the next one is queued, and the peripheral disabled at its
     * end, before the chip select is released. Sent through the generic
     * driver, the payload never reached the chip: every burst went out with
     * a payload of zeros.
     */
    SPI_TypeDef *spi = SPI1;

    gpio_clearPin(VOC_CS);
    delayUs(CS_SETUP_HOLD_US);

    spi->CR2 = sizeof(tx);
    spi->CR1 |= SPI_CR1_SPE;
    spi->CR1 |= SPI_CR1_CSTART;

    for (size_t i = 0; i < sizeof(tx); i++) {
        while ((spi->SR & SPI_SR_TXP) == 0)
            ;
        *((volatile uint8_t *)&spi->TXDR) = tx[i];
        while ((spi->SR & SPI_SR_RXP) == 0)
            ;
        (void)*((volatile uint8_t *)&spi->RXDR);
    }

    while ((spi->SR & SPI_SR_EOT) == 0)
        ;
    spi->IFCR = SPI_IFCR_EOTC | SPI_IFCR_TXTFC;
    spi->CR1 &= ~SPI_CR1_SPE;

    delayUs(CS_SETUP_HOLD_US);
    gpio_setPin(VOC_CS);
}

#endif /* CONFIG_DMR */
