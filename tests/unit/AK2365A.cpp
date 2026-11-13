/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "drivers/baseband/AK2365A.h"
#include "drivers/SPI/spi_bitbang.h"

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
 * Fake gpio set: the chip select (pin 0) latches the word.
 */
static void fakeSet(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev;

    if (pin == 0)
        words.push_back(shiftReg);
}

/**
 * Fake gpio clear: the chip select (pin 0) starts a new word.
 */
static void fakeClear(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev;

    if (pin == 0)
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

/**
 * Fake three-wire read, for the probe: the bitbang driver only builds for
 * the radios, and the probe is not tested here.
 */
uint32_t spiBitbang_read3wire(const struct spiCustomDevice *dev,
                              const struct gpioPin *cs, const uint32_t header,
                              const uint8_t headerBits, const uint16_t pull,
                              const uint32_t settleUs, const uint8_t bits)
{
    (void)dev;
    (void)cs;
    (void)header;
    (void)headerBits;
    (void)pull;
    (void)settleUs;
    (void)bits;

    return 0;
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

// Chip select on pin 0, reset on pin 1
static const struct ak2365a ifChip = {
    &fakeSpi,
    { &fakeGpio, 0 },
    { &fakeGpio, 1 },
};

/**
 * Words written by a receive set-up.
 *
 * @param bw: IF filter bandwidth.
 * @param wide: wide demodulated level.
 * @return the words, in write order.
 */
static std::vector<uint16_t> setupRx(const enum AK2365A_BPF bw, const bool wide)
{
    words.clear();
    shiftReg = 0;
    AK2365A_setupRx(&ifChip, bw, wide);

    return words;
}

/*
 * Expected words: RadioFoundry's for the MD-9600 V1-V3,
 * docs/spec/hardware-md9600.md section 5.4,
 * https://github.com/radiofoundry/radiofoundry. The reset releases the chip
 * select first: an empty word.
 */
TEST_CASE("AK2365A receive set-up matches RadioFoundry", "[ak2365a]")
{
    SECTION("25 kHz channel")
    {
        const std::vector<uint16_t> exp = { 0x0000, 0x08AA, 0x02F1, 0x041D,
                                            0x0600, 0x1601, 0x1880 };
        REQUIRE(setupRx(AK2365A_BPF_6, true) == exp);
    }

    SECTION("12.5 kHz channel")
    {
        const std::vector<uint16_t> exp = { 0x0000, 0x08AA, 0x02E9, 0x041D,
                                            0x0600, 0x1601, 0x1880 };
        REQUIRE(setupRx(AK2365A_BPF_3, false) == exp);
    }
}
