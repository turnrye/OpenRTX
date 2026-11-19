/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/nvmem.h"
#include "calibration/calibInfo_MD9600.h"
#include "core/nvmem_device.h"
#include "core/utils.h"
#include "drivers/NVM/W25Qx.h"
#include "drivers/baseband/radioUtils.h"
#include <stddef.h>

/**
 * Where one band's calibration lives in the security registers. Offsets from
 * RadioFoundry, docs/spec/hardware-md9600.md section 11.2,
 * https://github.com/radiofoundry/radiofoundry
 */
struct calLayout {
    uint16_t freqs;  ///< Frequency list: RX and TX pairs, BCD, 10 Hz units.
    uint16_t rxTune; ///< Front-end tuning row.
    uint16_t paDrive[MD9600_PA_LEVELS];     ///< PA drive rows, by power level.
    uint16_t fmModI;                        ///< FM two-point modulation row, I.
    uint16_t fmModQ;                        ///< FM two-point modulation row, Q.
    uint16_t dmrModI;                       ///< DMR two-point modulation row, I.
    uint16_t dmrModQ;                       ///< DMR two-point modulation row, Q.
    uint16_t ctcssLevel[MD9600_CTCSS_ROWS]; ///< CTCSS level rows, by tone.
    uint16_t refTune;                       ///< Reference oscillator offset.
    uint8_t points;                         ///< Number of calibration points.
};

/*
 * The power level to row mapping is the stock S126.061 firmware's
 * (0x0806eea4): rows 0x20, 0x1A0, 0x190, 0x10, plus 9 on VHF. The CTCSS
 * rows are the ones its tone level function reads (0x0805017c): 0x90,
 * 0xA0, 0xB0 for 67.0, 151.4 and 254.1 Hz, plus 0xB on VHF.
 */
static const struct calLayout calLayout[] = {
    [BND_VHF] = {
        .freqs   = 0x2000,
        .rxTune  = 0x1039,
        .paDrive = { 0x1029, 0x20A9, 0x2099, 0x1019 },
        .fmModI  = 0x2079,
        .fmModQ  = 0x2089,
        .dmrModI = 0x2039,
        .dmrModQ = 0x2049,
        .ctcssLevel = { 0x109B, 0x10AB, 0x10BB },
        .refTune = 0x100C,
        .points  = MD9600_CAL_POINTS_VHF,
    },
    [BND_UHF] = {
        .freqs   = 0x20B0,
        .rxTune  = 0x1030,
        .paDrive = { 0x1020, 0x20A0, 0x2090, 0x1010 },
        .fmModI  = 0x2070,
        .fmModQ  = 0x2080,
        .dmrModI = 0x2030,
        .dmrModQ = 0x2040,
        .ctcssLevel = { 0x1090, 0x10A0, 0x10B0 },
        .refTune = 0x1009,
        .points  = MD9600_CAL_POINTS_UHF,
    },
};

static const struct W25QxCfg eflashCfg = {
    .spi = &spi2,
    .cs = { FLASH_CS },
};

/*
 * The device macros end with their own semicolon, which clang-format can't
 * see: it would join them with the table below.
 */
// clang-format off
W25Qx_DEVICE_DEFINE(eflash, eflashCfg)
W25Qx_SECREG_DEFINE(secReg, eflashCfg)

static const struct nvmDescriptor nvmDevices[] = {
    {
        .name = "External flash",
        .dev = &eflash,
        .baseAddr = 0x00000000,
        .size = 0x1000000, // 16 MB, 128 Mbit
        .nbPart = 0,
        .partitions = NULL,
    },
    {
        .name = "Cal. data 1",
        .dev = &secReg,
        .baseAddr = 0x1000,
        .size = 0x100, // 256 byte
        .nbPart = 0,
        .partitions = NULL,
    },
    {
        .name = "Cal. data 2",
        .dev = &secReg,
        .baseAddr = 0x2000,
        .size = 0x100, // 256 byte
        .nbPart = 0,
        .partitions = NULL,
    },
};
// clang-format on

const struct nvmTable nvmTab = {
    .areas = nvmDevices,
    .nbAreas = ARRAY_SIZE(nvmDevices),
};

/**
 * Read a calibration row. On the UHF band the factory measures the even
 * points only and the stock firmware rebuilds the odd ones as the mean of
 * their neighbours (S126.061 at 0x0802c5c8): do the same.
 *
 * @param row: destination.
 * @param addr: row address.
 * @param points: number of calibration points.
 */
static void readRow(uint8_t *row, const uint16_t addr, const uint8_t points)
{
    nvm_devRead(&secReg, addr, row, points);

    if (points != MD9600_CAL_POINTS_UHF)
        return;

    for (size_t i = 1; i < (size_t)(points - 1); i += 2)
        row[i] = (row[i - 1] + row[i + 1]) / 2;
}

/**
 * Read the calibration of one band.
 *
 * @param cal: destination.
 * @param layout: where the band's calibration is stored.
 */
static void readBandCalib(struct md9600BandCalib *cal,
                          const struct calLayout *layout)
{
    uint32_t freqs[2 * MD9600_CAL_POINTS_UHF];

    nvm_devRead(&secReg, layout->freqs, freqs,
                layout->points * 2 * sizeof(uint32_t));
    readRow(cal->rxTune, layout->rxTune, layout->points);
    readRow(cal->fmModI, layout->fmModI, layout->points);
    readRow(cal->fmModQ, layout->fmModQ, layout->points);
    readRow(cal->dmrModI, layout->dmrModI, layout->points);
    readRow(cal->dmrModQ, layout->dmrModQ, layout->points);
    nvm_devRead(&secReg, layout->refTune, &cal->refTune, 1);

    for (size_t i = 0; i < MD9600_PA_LEVELS; i++)
        readRow(cal->paDrive[i], layout->paDrive[i], layout->points);

    for (size_t i = 0; i < MD9600_CTCSS_ROWS; i++)
        readRow(cal->ctcssLevel[i], layout->ctcssLevel[i], layout->points);

    // Frequencies are stored in units of 10 Hz, RX first in each pair
    for (size_t i = 0; i < layout->points; i++)
        cal->rxFreq[i] = bcdToBin(freqs[2 * i]) * 10;
}

void nvm_init()
{
    W25Qx_init(&eflash);
}

void nvm_terminate()
{
    W25Qx_terminate(&eflash);
}

void nvm_readCalibData(void *buf)
{
    struct md9600Calib *cal = buf;

    readBandCalib(&cal->band[BND_VHF], &calLayout[BND_VHF]);
    readBandCalib(&cal->band[BND_UHF], &calLayout[BND_UHF]);
}

void nvm_readHwInfo(hwInfo_t *info)
{
    uint16_t uhfMin = 0;
    uint16_t uhfMax = 0;
    uint16_t vhfMin = 0;
    uint16_t vhfMax = 0;

    // Security register 3: base address 0x3000
    nvm_devRead(&secReg, 0x3000, info->name, 8);
    nvm_devRead(&secReg, 0x3014, &uhfMin, 2);
    nvm_devRead(&secReg, 0x3016, &uhfMax, 2);
    nvm_devRead(&secReg, 0x3018, &vhfMin, 2);
    nvm_devRead(&secReg, 0x301a, &vhfMax, 2);
    nvm_devRead(&secReg, 0x3035, &info->hw_version, 1);

    // Ensure correct null-termination of device name by removing the 0xff.
    for (size_t i = 0; i < sizeof(info->name); i++) {
        if (info->name[i] == 0xFF)
            info->name[i] = '\0';
    }

    // Band edges are stored in BCD, in units of 100 kHz
    info->uhf_minFreq = bcdToBin(uhfMin) / 10;
    info->uhf_maxFreq = bcdToBin(uhfMax) / 10;
    info->vhf_minFreq = bcdToBin(vhfMin) / 10;
    info->vhf_maxFreq = bcdToBin(vhfMax) / 10;
    info->vhf_band = 1;
    info->uhf_band = 1;
}
