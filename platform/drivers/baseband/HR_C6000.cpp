/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/utils.h"
#include "peripherals/gpio.h"
#include "peripherals/spi.h"
#include "interfaces/delays.h"
#include "drivers/baseband/HR_C6000.h"
#include <string.h>

/*
 * Table of HR_C6000 CTCSS tones, used for reverse lookup of tone index to be
 * written in the configuration register. Taken from datasheet at page 90.
 */
static const uint16_t ctcssToneTable[] =
{
    670,  719,  744,  770,  797,  825,  854,
    885,  915,  948,  974,  1000, 1035, 1072,
    1109, 1148, 1188, 1230, 1273, 1318, 1365,
    1413, 1462, 1514, 1567, 1622, 1679, 1738,
    1799, 1862, 1928, 2035, 2107, 2181, 2257,
    2336, 2418, 2503, 693,  625,  1598, 1655,
    1713, 1773, 1835, 1899, 1966, 1995, 2065,
    2291, 2541
};

static uint8_t getToneIndex(const tone_t tone)
{
    uint8_t idx;

    for(idx = 0; idx < ARRAY_SIZE(ctcssToneTable); idx += 1)
    {
        if(ctcssToneTable[idx] == tone)
            break;
    }

    return idx + 1;
}

void HR_C6000::setTxCtcss(const tone_t tone, const uint8_t deviation)
{
    uint8_t index = getToneIndex(tone);
    writeCfgRegister(0xA8, index);        // Set CTCSS tone index
    writeCfgRegister(0xA0, deviation);    // Set CTCSS tone deviation
    writeCfgRegister(0xA1, 0x08);         // Enable CTCSS
}

void HR_C6000::setRxCtcss(const tone_t tone)
{
    uint8_t index = getToneIndex(tone);
    writeCfgRegister(0xA1, 0x08);         // Enable CTCSS
    writeCfgRegister(0xA7, 0x10);         // CTCSS detection threshold, value from datasheet
    writeCfgRegister(0xD3, 0x07);         // CTCSS sampling depth, value from datasheet
    writeCfgRegister(0xD2, 0xD0);
    writeCfgRegister(0xD4, index);        // Tone index
}

void HR_C6000::sendTone(const uint32_t freq, const uint8_t deviation)
{
    uint32_t tone = (freq * 65536) / 32000;

    // Set  DTMF tone osc 1 to frequency of the required tone
    writeReg16(C6000_SpiOpModes::CONFIG, 0x11A, (tone & 0xFF));
    writeReg16(C6000_SpiOpModes::CONFIG, 0x11B, (tone >> 8) & 0xFF);

    // Set  DTMF tone osc 2 to frequency of the required tone
    writeReg16(C6000_SpiOpModes::CONFIG, 0x122, (tone & 0xFF));
    writeReg16(C6000_SpiOpModes::CONFIG, 0x123, (tone >> 8) & 0xFF);

    writeCfgRegister(0xA1, 0x02);         // Enable DTMF
    writeCfgRegister(0xA0, deviation);    // Set DTMF tone deviation
    writeCfgRegister(0xA4, 0xFF);         // Set the tone time to maximum
    writeCfgRegister(0xA3, 0x00);         // Set the tone gap to zero
    writeCfgRegister(0xD1, 0x06);         // Set the number of codes to six
    writeCfgRegister(0xAF, 0x11);         // Set the same code to be sent six times (2 codes per register)
    writeCfgRegister(0xAE, 0x11);
    writeCfgRegister(0xAD, 0x11);
    writeCfgRegister(0x60, 0x00);         // Disable FM transmission
    writeCfgRegister(0x60, 0x80);         // Enable FM transmission, start sending the tone
}

void HR_C6000::readDmrStatus(struct C6000_DmrStatus *status)
{
    status->intFlags   = readCfgRegister(0x82);
    status->rxStatus   = readCfgRegister(0x51);
    status->cach       = readCfgRegister(0x52);
    status->syncStatus = readCfgRegister(0x5F);
}

uint8_t HR_C6000::readCfgRegisterFromIsr(const uint8_t addr)
{
    uint8_t cmd[3];
    uint8_t ret[3];

    cmd[0] = SPI_FLAGS_READ | static_cast< uint8_t >(C6000_SpiOpModes::CONFIG);
    cmd[1] = addr;
    cmd[2] = 0x00;

    /*
     * Same transaction as readReg but with the chip select driven directly
     * instead of through ScopedChipSelect, which takes a mutex that cannot be
     * locked from interrupt context. The short settling delays around the
     * deassertion are the ones the RT3s was found to need.
     */
    gpioPin_clear(&uCs);
    spi_transfer(uSpi, cmd, ret, 3);
    delayUs(2);
    gpioPin_set(&uCs);
    delayUs(2);

    return ret[2];
}

void HR_C6000::writeCfgRegisterFromIsr(const uint8_t addr, const uint8_t value)
{
    uint8_t cmd[3];

    cmd[0] = static_cast< uint8_t >(C6000_SpiOpModes::CONFIG);
    cmd[1] = addr;
    cmd[2] = value;

    /*
     * Same transaction as writeReg but with the chip select driven directly
     * rather than through ScopedChipSelect, whose mutex cannot be locked from
     * interrupt context. The settling delays match readCfgRegisterFromIsr.
     */
    gpioPin_clear(&uCs);
    spi_send(uSpi, cmd, sizeof(cmd));
    delayUs(2);
    gpioPin_set(&uCs);
    delayUs(2);
}

void HR_C6000::writeDmrLinkControlFromIsr(const uint8_t *lc)
{
    uint8_t cmd[2 + C6000_LC_LENGTH];

    /*
     * Page write towards the transmit data register: the command byte selects
     * the DATA page with neither the read nor the extended addressing flag, the
     * register offset follows, then the twelve link control bytes in one go.
     */
    cmd[0] = static_cast< uint8_t >(C6000_SpiOpModes::DATA);
    cmd[1] = 0x00;
    memcpy(&cmd[2], lc, C6000_LC_LENGTH);

    gpioPin_clear(&uCs);
    spi_send(uSpi, cmd, sizeof(cmd));
    delayUs(2);
    gpioPin_set(&uCs);
    delayUs(2);
}

void HR_C6000::readDmrLinkControl(uint8_t *lc)
{
    readPage(C6000_SpiOpModes::DATA, 0x00, lc, C6000_LC_LENGTH);
}

void HR_C6000::readDmrVoicePayload(uint8_t *payload)
{
    readPage(C6000_SpiOpModes::DATA, 0x66, payload, C6000_VOICE_PAYLOAD_LENGTH);
}
