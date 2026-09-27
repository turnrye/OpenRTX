/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CALIBINFO_CS7000_H
#define CALIBINFO_CS7000_H

#include "core/datatypes.h"
#include <stdint.h>


/**
 * \brief Shape of the PA ramp of a DMR burst, at one output power level.
 *
 * The PA is ramped up and down for every burst, starting from the transmit
 * interrupt of the HR_C6000. Times are counted from that interrupt in units of
 * 100us.
 */
struct CS7000PaRamp
{
    uint8_t  txAdvance[8];       // 0x00: HR_C6000 register 0x12
    uint8_t  rampStart[8];       // 0x08: start of the ramp up
    uint8_t  rampUpEnd[8];       // 0x10: end of the ramp up
    uint16_t holdEnd[8];         // 0x18: end of the hold, little endian
    uint16_t rampDownEnd[8];     // 0x28: end of the ramp down, little endian
};

/**
 * \brief Calibration data for Connect Systems CS7000.
 */
struct CS7000Calib
{
    uint32_t txCalFreq[8];       // 0x000
    uint32_t rxCalFreq[8];       // 0x024
    uint8_t  rxSensitivity[8];   // 0x044
    uint8_t  txHighPwr[8];       // 0x06C
    uint8_t  txMiddlePwr[8];     // 0x074
    uint8_t  dmrMod2Amp[8];      // 0x0B4: DMR modulation, register 0x45
    uint8_t  dmrMod1Amp[8];      // 0x0BC: DMR modulation, register 0x46
    uint8_t  txDigitalPathQ[8];  // 0x0C4
    uint8_t  txAnalogPathI[8];   // 0x0CC
    uint8_t  txAnalogPathQ[8];   // 0x0D4
    uint8_t  errorRate[8];       // 0x0DC
    struct CS7000PaRamp paRampHigh;  // 0x0EC: DMR PA ramp, high power
    struct CS7000PaRamp paRampLow;   // 0x124: DMR PA ramp, low power
    uint8_t  dmrTxBias;          // 0x174: DMR modulation bias, register 0x04
};

/**
 * \brief RSSI calibration data.
 */
struct rssiParams
{
    float    slope;
    float    offset;
    uint32_t rxFreq;
};

#endif /* CALIBINFO_CS7000_H */
