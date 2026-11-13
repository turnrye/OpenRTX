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
    uint16_t freqs;   ///< Frequency list: RX and TX pairs, BCD, 10 Hz units.
    uint16_t rxTune;  ///< Front-end tuning row.
    uint16_t refTune; ///< Reference oscillator offset.
    uint8_t points;   ///< Number of calibration points.
};

static const struct calLayout calLayout[] = {
    [BND_VHF] = {
        .freqs   = 0x2000,
        .rxTune  = 0x1039,
        .refTune = 0x100C,
        .points  = MD9600_CAL_POINTS_VHF,
    },
    [BND_UHF] = {
        .freqs   = 0x20B0,
        .rxTune  = 0x1030,
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
    nvm_devRead(&secReg, layout->rxTune, cal->rxTune, layout->points);
    nvm_devRead(&secReg, layout->refTune, &cal->refTune, 1);

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
    nvm_devRead(&secReg, 0x1004, &cal->rssiLow, 1);
    nvm_devRead(&secReg, 0x1005, &cal->rssiHigh, 1);
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
