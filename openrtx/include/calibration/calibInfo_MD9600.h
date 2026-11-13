/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CALIBINFO_MD9600_H
#define CALIBINFO_MD9600_H

#include <stdint.h>

/**
 * Number of calibration points per band.
 */
enum md9600CalPoints {
    MD9600_CAL_POINTS_UHF = 9, ///< Points of the UHF band.
    MD9600_CAL_POINTS_VHF = 5, ///< Points of the VHF band.
};

/**
 * Factory calibration of one band of the TYT MD-9600.
 */
struct md9600BandCalib {
    uint32_t rxFreq[MD9600_CAL_POINTS_UHF]; ///< RX calibration points, in Hz.
    uint8_t rxTune[MD9600_CAL_POINTS_UHF];  ///< Front-end tuning DAC, 8 bit.
    uint8_t refTune; ///< Reference oscillator offset, stored + 128.
};

/**
 * Factory calibration of the TYT MD-9600, from the security registers 1 and
 * 2 of the SPI flash. Layout from RadioFoundry, docs/spec/hardware-md9600.md
 * section 11.2, https://github.com/radiofoundry/radiofoundry
 */
struct md9600Calib {
    struct md9600BandCalib band[2]; ///< Indexed by enum Band.
    uint8_t rssiLow;                ///< RSSI reading at -120 dBm.
    uint8_t rssiHigh;               ///< RSSI reading at -70 dBm.
};

#endif /* CALIBINFO_MD9600_H */
