/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "drivers/baseband/LMX2571.h"

/*
 * Words captured from the bus: bytes are collected while the chip select is
 * low, and the word is latched when it rises.
 */
static std::vector<uint32_t> words;
static uint32_t shiftReg;

/**
 * Fake SPI transfer: shift the bytes into the current word.
 */
static int fakeTransfer(const struct spiDevice *dev, const void *txBuf,
                        void *rxBuf, const size_t size)
{
    (void)dev;
    (void)rxBuf;

    const uint8_t *data = static_cast<const uint8_t *>(txBuf);
    for (size_t i = 0; i < size; i++)
        shiftReg = (shiftReg << 8) | data[i];

    return 0;
}

/**
 * Fake gpio mode setting: nothing to do.
 */
static int fakeMode(const struct gpioDev *dev, const uint8_t pin,
                    const uint16_t mode)
{
    (void)dev;
    (void)pin;
    (void)mode;

    return 0;
}

/**
 * Fake chip select release: latch the word.
 */
static void fakeSet(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev;
    (void)pin;

    words.push_back(shiftReg & 0xFFFFFF);
}

/**
 * Fake chip select assertion: start a new word.
 */
static void fakeClear(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev;
    (void)pin;

    shiftReg = 0;
}

/**
 * Fake gpio read: never used.
 */
static bool fakeRead(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev;
    (void)pin;

    return false;
}

// C++14 has no designated initializers: fields in declaration order
static const struct gpioApi fakeGpioApi = {
    fakeMode,  // mode
    fakeSet,   // set
    fakeClear, // clear
    fakeRead,  // read
};

static const struct gpioDev fakeGpio = {
    &fakeGpioApi, // api
    nullptr,      // priv
};

static const struct spiDevice fakeSpi = {
    fakeTransfer, // transfer
    nullptr,      // priv
    nullptr,      // mutex
};

/*
 * MD-9600 V5 settings, as written by the stock S126.061 firmware: 16.8 MHz
 * reference, pre-divider 8 on VHF and 4 on UHF.
 */
static const struct lmx2571Config vhfCfg = {
    16800000, // refClk
    0x003E,   // r0
    0x8581,   // r6
    0x1024,   // r7
    0x0430,   // r8
    0x0230,   // r41
    0x0108,   // r42
    8,        // refDiv
    3,        // fracOrder
};

static const struct lmx2571Config uhfCfg = {
    16800000, // refClk
    0x003E,   // r0
    0x8581,   // r6
    0x1024,   // r7
    0x0430,   // r8
    0x0230,   // r41
    0x0108,   // r42
    4,        // refDiv
    3,        // fracOrder
};

static const struct lmx2571 vhfPll = { &fakeSpi, &vhfCfg, { &fakeGpio, 0 } };
static const struct lmx2571 uhfPll = { &fakeSpi, &uhfCfg, { &fakeGpio, 0 } };

/**
 * The words S126.061 writes for one tune, given the PLL_N, fraction and
 * pre-divider fields.
 */
static std::vector<uint32_t> stockWords(uint32_t n, uint32_t frac,
                                        uint32_t refDiv)
{
    return {
        0x00003E,
        0x043000 | n,
        0x010000 | (frac >> 16),
        0x020000 | (frac & 0xFFFF),
        0x030000,
        0x050100 | refDiv,
        0x068581,
        0x071024,
        0x080430,
        0x290230,
        0x2A0108,
        0x00003F,
        0x00003E,
    };
}

TEST_CASE("LMX2571 reset", "[lmx2571]")
{
    // Initialisation releases the chip select, which latches an empty word
    words.clear();
    LMX2571_init(&vhfPll);
    REQUIRE(words.back() == 0x002000);
}

TEST_CASE("LMX2571 tuning matches the stock MD-9600 firmware", "[lmx2571]")
{
    SECTION("VHF, half fraction")
    {
        // 196.35 MHz = 93.5 x 2.1 MHz
        words.clear();
        LMX2571_setFrequency(&vhfPll, 196350000);
        REQUIRE(words == stockWords(93, 0x800000, 8));
    }

    SECTION("UHF, quarter fraction")
    {
        // 395.85 MHz = 94.25 x 4.2 MHz
        words.clear();
        LMX2571_setFrequency(&uhfPll, 395850000);
        REQUIRE(words == stockWords(94, 0x400000, 4));
    }

    SECTION("Fraction rounded to the nearest step")
    {
        // 196.47 MHz / 2.1 MHz = 93.557142857..., fraction 9347306.06
        words.clear();
        LMX2571_setFrequency(&vhfPll, 196470000);
        REQUIRE(words == stockWords(93, 9347306, 8));
    }
}
