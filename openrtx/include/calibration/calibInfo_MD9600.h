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
 * Transmit power levels, one PA drive row each, lowest first. The factory
 * labels them 5, 10, 25 and 40 W; the output they give is not measured.
 */
enum md9600PaLevel {
    MD9600_PA_LEVEL_5W = 0,  ///< Lowest drive.
    MD9600_PA_LEVEL_10W = 1, ///< Second drive.
    MD9600_PA_LEVEL_25W = 2, ///< Third drive.
    MD9600_PA_LEVEL_40W = 3, ///< Highest drive.
    MD9600_PA_LEVELS = 4,    ///< Number of levels.
};

/**
 * CTCSS level rows, by the tone each was set at.
 */
enum md9600CtcssRow {
    MD9600_CTCSS_67_0 = 0,  ///< 67.0 Hz.
    MD9600_CTCSS_151_4 = 1, ///< 151.4 Hz.
    MD9600_CTCSS_254_1 = 2, ///< 254.1 Hz.
    MD9600_CTCSS_ROWS = 3,  ///< Number of rows.
};

/**
 * Factory calibration of one band of the TYT MD-9600.
 */
struct md9600BandCalib {
    uint32_t rxFreq[MD9600_CAL_POINTS_UHF]; ///< Calibration points, in Hz.
    uint8_t rxTune[MD9600_CAL_POINTS_UHF];  ///< Front-end tuning DAC, 8 bit.
    uint8_t paDrive[MD9600_PA_LEVELS][MD9600_CAL_POINTS_UHF]; ///< APC DAC.
    uint8_t fmModI[MD9600_CAL_POINTS_UHF]; ///< FM two-point modulation, I.
    uint8_t fmModQ[MD9600_CAL_POINTS_UHF]; ///< FM two-point modulation, Q.
    /// DMR two-point modulation, I (MOD2) and Q (MOD1), set for a 1944 Hz
    /// outer deviation.
    uint8_t dmrModI[MD9600_CAL_POINTS_UHF];
    uint8_t dmrModQ[MD9600_CAL_POINTS_UHF];
    /// CTCSS peak-to-peak PWM swing at 12.5 kHz, by tone row.
    uint8_t ctcssLevel[MD9600_CTCSS_ROWS][MD9600_CAL_POINTS_UHF];
    uint8_t refTune; ///< Reference oscillator offset, stored + 128.
};

/**
 * Factory calibration of the TYT MD-9600, from the security registers 1 and
 * 2 of the SPI flash. Layout from RadioFoundry, docs/spec/hardware-md9600.md
 * section 11.2, https://github.com/radiofoundry/radiofoundry
 */
struct md9600Calib {
    struct md9600BandCalib band[2]; ///< Indexed by enum Band.
};

#endif /* CALIBINFO_MD9600_H */
