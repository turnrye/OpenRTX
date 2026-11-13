/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "drivers/baseband/SKY72310.h"

/*
 * Words captured from the bus: bytes are collected while the chip select is
 * low, and the word is latched when it rises.
 */
static std::vector<uint16_t> words;
static uint16_t shiftReg;

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

    words.push_back(shiftReg);
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

// MD-9600 V1-V3: 16.8 MHz reference
static const struct sky73210 pll = { &fakeSpi, { &fakeGpio, 0 }, 16800000 };

static constexpr uint8_t REF_DIV_VHF = 6;
static constexpr uint8_t REF_DIV_UHF = 4;

/**
 * Words written to tune a VCO frequency.
 *
 * @param freq: VCO frequency, in Hz.
 * @param clkDiv: reference clock divider.
 * @return the words, in write order.
 */
static std::vector<uint16_t> tune(const uint32_t freq, const uint8_t clkDiv)
{
    words.clear();
    SKY73210_setFrequency(&pll, freq, clkDiv);

    return words;
}

/*
 * Expected words: RadioFoundry's sky72310_program_words() for the MD-9600
 * V1-V3 receive LO (VHF: twice f + 49.95 MHz, R 6; UHF: f - 49.95 MHz, R 4),
 * https://github.com/radiofoundry/radiofoundry. The driver writes the
 * divider, the dividend LSB and MSB, then the reference divider.
 */
TEST_CASE("SKY72310 setup words", "[sky72310]")
{
    // Initialisation releases the chip select first: an empty word
    const std::vector<uint16_t> exp = { 0x0000, 0x601F, 0x73D0, 0x8000,
                                        0x9000 };

    words.clear();
    shiftReg = 0;
    SKY73210_init(&pll, 31);
    REQUIRE(words == exp);
}

TEST_CASE("SKY72310 tuning matches RadioFoundry on the MD-9600", "[sky72310]")
{
    SECTION("VHF 146.520 MHz")
    {
        const std::vector<uint16_t> exp = { 0x006C, 0x20C5, 0x1157, 0x5005 };
        REQUIRE(tune(392940000, REF_DIV_VHF) == exp);
    }

    SECTION("VHF 145.000 MHz, fraction of a quarter")
    {
        const std::vector<uint16_t> exp = { 0x006B, 0x2000, 0x1100, 0x5005 };
        REQUIRE(tune(389900000, REF_DIV_VHF) == exp);
    }

    SECTION("VHF 151.5125 MHz, negative fraction")
    {
        const std::vector<uint16_t> exp = { 0x0070, 0x206E, 0x139B, 0x5005 };
        REQUIRE(tune(402925000, REF_DIV_VHF) == exp);
    }

    SECTION("UHF 446.000 MHz")
    {
        const std::vector<uint16_t> exp = { 0x003E, 0x20C3, 0x1130, 0x5003 };
        REQUIRE(tune(396050000, REF_DIV_UHF) == exp);
    }

    SECTION("UHF 446.125 MHz")
    {
        const std::vector<uint16_t> exp = { 0x003E, 0x203D, 0x114F, 0x5003 };
        REQUIRE(tune(396175000, REF_DIV_UHF) == exp);
    }

    SECTION("UHF 440.000 MHz, negative fraction")
    {
        const std::vector<uint16_t> exp = { 0x003D, 0x20E8, 0x1379, 0x5003 };
        REQUIRE(tune(390050000, REF_DIV_UHF) == exp);
    }
}
