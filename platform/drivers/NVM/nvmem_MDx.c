/***************************************************************************
 *   Copyright (C) 2020 - 2025 by Federico Amedeo Izzo IU2NUO,             *
 *                                Niccolò Izzo IU2KIN                      *
 *                                Frederik Saraci IU2NRO                   *
 *                                Silvano Seva IU2KWO                      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, see <http://www.gnu.org/licenses/>   *
 ***************************************************************************/

#include "interfaces/nvmem.h"
#include "interfaces/delays.h"
#include "calibration/calibInfo_MDx.h"
#include "core/nvmem_access.h"
#include "drivers/SPI/spi_stm32.h"
#include <string.h>
#include "wchar.h"
#include "core/utils.h"
#include "drivers/NVM/W25Qx.h"
#include "drivers/NVM/eeep.h"
#include "core/crc.h"

#define SECREG_READ(dev, offs, data, len) \
    nvm_devRead((const struct nvmDevice *) dev, offs, data, len)

static const struct W25QxCfg eflashCfg =
{
    #ifdef PLATFORM_MD9600
    .spi = &spi2,
    #else
    .spi = &nvm_spi,
    #endif
    .cs  = { FLASH_CS }
};

W25Qx_DEVICE_DEFINE(eflash, eflashCfg, 0x1000000)        // 16 MB, 128 Mbit
W25Qx_SECREG_DEFINE(cal1,   eflashCfg, 0x1000, 0x100)    // 256 byte
W25Qx_SECREG_DEFINE(cal2,   eflashCfg, 0x2000, 0x100)    // 256 byte
W25Qx_SECREG_DEFINE(hwInfo, eflashCfg, 0x3000, 0x100)    // 256 byte

EEEP_DEVICE_DEFINE(eeep)


const struct nvmPartition eflashPartitions[] = 
{
    {
        .offset = 0x0000,    // Calibration data (keep existing)
        .size   = 0x100000   // 1MB for cal data
    },
    {
        .offset = 0x100000,  // EEEP storage partition
        .size   = 0x10000    // 64kB for settings // TODO change to 16kB
    },
    {
        .offset = 0x110000,  // Remaining space
        .size   = 0xEF0000   // Rest of 16MB flash
    }
};

static const struct nvmDescriptor nvmDevices[] =
{
    {
        .name       = "External flash",
        .dev        = &eflash,
        .partNum    = 3,
        .partitions = eflashPartitions  // Add this
    },
    {
        .name       = "Cal. data 1",
        .dev        = (const struct nvmDevice *) &cal1,
        .partNum    = 0,
        .partitions = NULL
    },
    {
        .name       = "Cal. data 2",
        .dev        = (const struct nvmDevice *) &cal2,
        .partNum    = 0,
        .partitions = NULL
    },
    {
        .name       = "Virtual EEPROM", 
        .dev        = &eeep,
        .partNum    = 0,
        .partitions = NULL
    }
};


static uint16_t settingsCrc;
static uint16_t vfoCrc;

void nvm_init()
{
    #ifndef PLATFORM_MD9600
    gpio_setMode(FLASH_CLK, ALTERNATE | ALTERNATE_FUNC(5));
    gpio_setMode(FLASH_SDO, ALTERNATE | ALTERNATE_FUNC(5));
    gpio_setMode(FLASH_SDI, ALTERNATE | ALTERNATE_FUNC(5));

    spiStm32_init(&nvm_spi, 21000000, 0);
    #endif

    W25Qx_init(&eflash);
    eeep_init(&eeep, 0, 1); // Use appropriate partition
}

void nvm_terminate()
{
    eeep_terminate(&eeep);
    W25Qx_terminate(&eflash);
}

const struct nvmDescriptor *nvm_getDesc(const size_t index)
{
    if(index >= ARRAY_SIZE(nvmDevices))
        return NULL;

    return &nvmDevices[index];
}

void nvm_readCalibData(void *buf)
{
    uint32_t freqs[18];

    // Common calibration data between single and dual-band radios
    struct CalData *calib = ((struct CalData *) buf);

    // Security register 1: base address 0x1000
    SECREG_READ(&cal1, 0x0009, &(calib->freqAdjustMid), 1);
    SECREG_READ(&cal1, 0x0010, calib->txHighPower, 9);
    SECREG_READ(&cal1, 0x0020, calib->txLowPower, 9);
    SECREG_READ(&cal1, 0x0030, calib->rxSensitivity, 9);

    // Security register 2: base address 0x2000
    SECREG_READ(&cal2, 0x0030, calib->sendIrange, 9);
    SECREG_READ(&cal2, 0x0040, calib->sendQrange, 9);
    SECREG_READ(&cal2, 0x0070, calib->analogSendIrange, 9);
    SECREG_READ(&cal2, 0x0080, calib->analogSendQrange, 9);
    SECREG_READ(&cal2, 0x00b0, ((uint8_t *) &freqs), 72);

    /*
     * Frequency stored in calibration data is divided by ten: so, after
     * bcdToBin conversion, we have something like 40'135'000. To ajdust
     * things, frequency has to be multiplied by ten.
     */
    for(uint8_t i = 0; i < 9; i++)
    {
        calib->rxFreq[i] = ((freq_t) bcdToBin(freqs[2*i])) * 10;
        calib->txFreq[i] = ((freq_t) bcdToBin(freqs[2*i+1])) * 10;
    }

    // Calibration data for dual-band radios only
    #ifndef PLATFORM_MD3x0
    mduv3x0Calib_t *cal = (mduv3x0Calib_t *) buf;
    struct CalData *vhfCal = &(cal->vhfCal);

    // Security register 1: base address 0x1000
    SECREG_READ(&cal1, 0x000c, (&vhfCal->freqAdjustMid), 1);
    SECREG_READ(&cal1, 0x0019, vhfCal->txHighPower, 5);
    SECREG_READ(&cal1, 0x0029, vhfCal->txLowPower, 5);
    SECREG_READ(&cal1, 0x0039, vhfCal->rxSensitivity, 5);

    // Security register 2: base address 0x2000
    SECREG_READ(&cal2, 0x0039, vhfCal->sendIrange, 5);
    SECREG_READ(&cal2, 0x0049, vhfCal->sendQrange, 5);
    SECREG_READ(&cal2, 0x0079, vhfCal->analogSendIrange, 5);
    SECREG_READ(&cal2, 0x0089, vhfCal->analogSendQrange, 5);
    SECREG_READ(&cal2, 0x0000, ((uint8_t *) &freqs), 40);

    for(uint8_t i = 0; i < 5; i++)
    {
        vhfCal->rxFreq[i] = ((freq_t) bcdToBin(freqs[2*i]));
        vhfCal->txFreq[i] = ((freq_t) bcdToBin(freqs[2*i+1]));
    }
    #endif
}

void nvm_readHwInfo(hwInfo_t *info)
{
    uint16_t freqMin = 0;
    uint16_t freqMax = 0;
    uint8_t  lcdInfo = 0;

    // Security register 3: base address 0x3000
    SECREG_READ(&hwInfo, 0x0000, info->name, 8);
    SECREG_READ(&hwInfo, 0x0014, &freqMin, 2);
    SECREG_READ(&hwInfo, 0x0016, &freqMax, 2);
    SECREG_READ(&hwInfo, 0x001D, &lcdInfo, 1);

    // Ensure correct null-termination of device name by removing the 0xff.
    for(uint8_t i = 0; i < sizeof(info->name); i++)
    {
        if(info->name[i] == 0xFF)
            info->name[i] = '\0';
    }

    freqMin = ((uint16_t) bcdToBin(freqMin))/10;
    freqMax = ((uint16_t) bcdToBin(freqMax))/10;

    info->hw_version = lcdInfo & 0x03;

    #ifdef PLATFORM_MD3x0
    // Single band device, either VHF or UHF
    if(freqMin < 200)
    {
        info->vhf_maxFreq = freqMax;
        info->vhf_minFreq = freqMin;
        info->vhf_band    = 1;
    }
    else
    {
        info->uhf_maxFreq = freqMax;
        info->uhf_minFreq = freqMin;
        info->uhf_band    = 1;
    }
    #else
    // For dual band devices load the remaining data
    uint16_t vhf_freqMin = 0;
    uint16_t vhf_freqMax = 0;

    SECREG_READ(&hwInfo, 0x0018, &vhf_freqMin, 2);
    SECREG_READ(&hwInfo, 0x001a, &vhf_freqMax, 2);

    info->vhf_minFreq = ((uint16_t) bcdToBin(vhf_freqMin))/10;
    info->vhf_maxFreq = ((uint16_t) bcdToBin(vhf_freqMax))/10;
    info->uhf_minFreq = freqMin;
    info->uhf_maxFreq = freqMax;
    info->vhf_band = 1;
    info->uhf_band = 1;
    #endif
}

int nvm_readSettings(uint8_t *settings, size_t len)
{
    memset(settings, 0x00, len);
    int ret = nvm_read(3, -1, NVM_SETTINGS_BASE, settings, len);
    if(ret < 0)
        return -1;

    settingsCrc = crc_ccitt(settings, len);

    return 0;
}

int nvm_writeSettingsSlice(uint8_t *slice, size_t len, size_t offset)
{
    return nvm_write(3, -1, NVM_SETTINGS_BASE + offset, slice, len);
}


/**
 * TODO: functions temporarily implemented in "nvmem_settings_MDx.c"

int nvm_readVFOChannelData(channel_t *channel)
int nvm_readSettings(uint8_t *settings, size_t len)
int nvm_writeSettings(const settings_t *settings)

*/
