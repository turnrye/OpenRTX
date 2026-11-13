/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/delays.h"
#include "drivers/baseband/AK2365A.h"
#include "drivers/SPI/spi_bitbang.h"
#include "core/utils.h"
#include <errno.h>

/*
 * Read test, from RadioFoundry (boards/md9600/rfplan_md9600.h): register 0x0B
 * (AGC 1 gain) is read-write and restored by the software reset. A read frame
 * is the R/W bit and the 6-bit address, then ten samples: the data sheet puts
 * D7-D0 in eight of them after one unstable bit.
 */
enum
{
    PROBE_REG       = 0x0B,
    PROBE_READ_BITS = 10,
    PROBE_SETTLE_US = 20,   // Released data line, through the pin's pull
    PROBE_PULL_US   = 50,   // Reset line, through the pin's pull
};

/*
 * Timings: reset low for at least 1us (data sheet section 12), and 500us
 * from every stage on to the start of the calibration (section 16).
 */
enum
{
    RESET_US     = 100,
    CAL_READY_US = 500,
};

static const uint8_t probePatterns[] = { 0x5A, 0xA5 };

static inline void writeReg(const struct ak2365a *dev, uint8_t reg, uint8_t value)
{
    uint8_t data[2];

    data[0] = (reg & 0x3f) << 1;
    data[1] = value;

    gpioPin_clear(&dev->cs);
    spi_send(dev->spi, &data, 2);
    gpioPin_set(&dev->cs);
}

/**
 * Hardware reset: chip select released, reset pulsed low.
 */
static void reset(const struct ak2365a *dev)
{
    gpioPin_setMode(&dev->cs, OUTPUT);
    gpioPin_setMode(&dev->res, OUTPUT);

    gpioPin_clear(&dev->res);
    gpioPin_set(&dev->cs);

    delayUs(RESET_US);
    gpioPin_set(&dev->res);
}

/**
 * Read a register frame: the raw samples, with the data line under a pull.
 */
static uint16_t readRaw(const struct ak2365a *dev, uint8_t reg, uint16_t pull)
{
    const struct spiCustomDevice *bus = (const struct spiCustomDevice *) dev->spi;
    const uint32_t header = 0x40 | (reg & 0x3F);

    return spiBitbang_read3wire(bus, &dev->cs, header, 7, pull, PROBE_SETTLE_US,
                                PROBE_READ_BITS);
}

/**
 * Whether the samples hold every pattern, under both pulls, at the same bit
 * position.
 */
static bool probeAnswered(uint16_t samples[2][ARRAY_SIZE(probePatterns)])
{
    for(uint8_t shift = 0; shift + 8 <= PROBE_READ_BITS; shift++)
    {
        bool all = true;

        for(uint8_t pull = 0; pull < 2; pull++)
        {
            for(uint8_t i = 0; i < ARRAY_SIZE(probePatterns); i++)
            {
                if(((samples[pull][i] >> shift) & 0xFF) != probePatterns[i])
                    all = false;
            }
        }

        if(all)
            return true;
    }

    return false;
}

/**
 * Whether every sample followed the pull: all zeros pulled down, all ones
 * pulled up.
 */
static bool probeFloated(uint16_t samples[2][ARRAY_SIZE(probePatterns)])
{
    const uint16_t ones = (1 << PROBE_READ_BITS) - 1;

    for(uint8_t i = 0; i < ARRAY_SIZE(probePatterns); i++)
    {
        if((samples[0][i] != 0) || (samples[1][i] != ones))
            return false;
    }

    return true;
}


void AK2365A_init(const struct ak2365a *dev)
{
    reset(dev);

    writeReg(dev, 0x04, 0xAA);   // Software reset
    writeReg(dev, 0x01, 0xC1);   // Operating mode 6, LO freq 50.4MHz
    writeReg(dev, 0x02, 0x33);   // Enable calibration
    writeReg(dev, 0x03, 0x00);   // IF buffer gain 5dB
    writeReg(dev, 0x0B, 0x01);   // AGC auto, AGC1 gain 21dB
    writeReg(dev, 0x0C, 0x80);   // AGC2 gain 12dB
}

int AK2365A_probe(const struct ak2365a *dev)
{
    uint16_t samples[2][ARRAY_SIZE(probePatterns)];

    gpioPin_setMode(&dev->cs, OUTPUT);
    gpioPin_set(&dev->cs);

    // Reset pulse through the pull resistors only
    gpioPin_setMode(&dev->res, INPUT_PULL_DOWN);
    delayUs(PROBE_PULL_US);
    gpioPin_setMode(&dev->res, INPUT_PULL_UP);
    delayUs(PROBE_PULL_US);

    for(uint8_t i = 0; i < ARRAY_SIZE(probePatterns); i++)
    {
        writeReg(dev, PROBE_REG, probePatterns[i]);
        samples[0][i] = readRaw(dev, PROBE_REG, INPUT_PULL_DOWN);
        samples[1][i] = readRaw(dev, PROBE_REG, INPUT_PULL_UP);
    }

    writeReg(dev, 0x04, 0xAA);   // Software reset

    if(probeAnswered(samples))
        return 0;

    if(probeFloated(samples))
        return -ENODEV;

    return -EIO;
}

void AK2365A_terminate(const struct ak2365a *dev)
{
    gpioPin_clear(&dev->res);
}

void AK2365A_setFilterBandwidth(const struct ak2365a *dev, const uint8_t bw)
{
    // BPF_BW[1:0] in register 0x01, BPF_BW[2] (F0 select) in register 0x0B.
    uint8_t reg01 = 0xE1 | ((bw & 0x03) << 2);
    uint8_t reg0B = 0x01 | ((bw & 0x04) << 5);

    writeReg(dev, 0x01, reg01);  // Operating mode 7, LO freq. 50.4MHz
    delayMs(1);
    writeReg(dev, 0x01, reg01);  // Operating mode 7, LO freq. 50.4MHz
    writeReg(dev, 0x02, 0x1E);   // AGC time = 3, AGC step 2dB
    writeReg(dev, 0x03, 0x00);   // IF buffer gain 5dB
    writeReg(dev, 0x0B, reg0B);  // BPF_BW[2], AGC auto
    writeReg(dev, 0x0C, 0x80);   // AGC2 gain 12dB
}

void AK2365A_setupRx(const struct ak2365a *dev, const enum AK2365A_BPF bw,
                     const bool wide)
{
    uint8_t reg01 = AK2365A_REG01_BS(7)          // Mode 7, every stage on
                  | AK2365A_REG01_BPF_BW(bw)
                  | AK2365A_REG01_LOFREQ(1);     // 50.4MHz
    if(wide)
        reg01 |= AK2365A_REG01_BAND;

    reset(dev);
    writeReg(dev, 0x04, AK2365A_REG04_SRST);
    writeReg(dev, 0x01, reg01);
    delayUs(CAL_READY_US);

    // AGC at the recommended speed: AGC_FAST and AGC1_STEP clear, AGC_TIME 3
    writeReg(dev, 0x02, AK2365A_REG02_RSVD4
                      | AK2365A_REG02_AGC_TIME(3)
                      | AK2365A_REG02_CAL);
    writeReg(dev, 0x03, AK2365A_REG03_IFOG(0));  // 6dB

    // Automatic gain control, manual gains at their reset values
    writeReg(dev, 0x0B, AK2365A_REG0B_BPF_BW2(bw) | AK2365A_REG0B_AGC1_G(1));
    writeReg(dev, 0x0C, AK2365A_REG0C_RSVD7 | AK2365A_REG0C_AGC2_G(0));
}
