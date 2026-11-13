/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef LMX2571_H
#define LMX2571_H

#include "peripherals/gpio.h"
#include "peripherals/spi.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Board-specific settings of an LMX2571 used as a PLL with an external VCO.
 */
struct lmx2571Config {
    uint32_t refClk;   ///< Reference clock frequency, in Hz.
    uint16_t r0;       ///< R0 in normal operation, FCAL_EN clear.
    uint16_t r6;       ///< R6: loop filter, channel dividers, multiplier.
    uint16_t r7;       ///< R7: output buffers, loop filter.
    uint16_t r8;       ///< R8: FSK mode, external VCO selection.
    uint16_t r41;      ///< R41: external VCO charge pump.
    uint16_t r42;      ///< R42: external VCO charge pump.
    uint8_t refDiv;    ///< PLL_R_PRE: phase detector at refClk / refDiv.
    uint8_t fracOrder; ///< Order of the delta-sigma modulator.
};

/**
 * LMX2571 device descriptor.
 */
struct lmx2571 {
    const struct spiDevice *spi;     ///< SPI bus device driver.
    const struct lmx2571Config *cfg; ///< Board-specific settings.
    const struct gpioPin cs;         ///< Chip select gpio.
};

/**
 * Initialise the chip select line and reset every register of the chip to
 * its default value.
 *
 * @param dev: pointer to device descriptor.
 */
void LMX2571_init(const struct lmx2571 *dev);

/**
 * Program the synthesizer to a new output frequency and start the loop
 * calibration. The fraction is taken over 2^24, as the chip requires in FSK
 * mode with PLL_DEN set to zero.
 *
 * @param dev: pointer to device descriptor.
 * @param freq: output frequency, in Hz.
 */
void LMX2571_setFrequency(const struct lmx2571 *dev, const uint32_t freq);

#ifdef __cplusplus
}
#endif

#endif /* LMX2571_H */
