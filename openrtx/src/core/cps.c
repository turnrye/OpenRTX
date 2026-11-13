/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/platform.h"
#include "core/cps.h"
#include "core/utils.h"

/*
 * Transmit power levels the radio offers, in mW, lowest first. A target
 * defines CONFIG_TX_POWER_LEVELS in its hwconfig.h; the default suits a 5 W
 * handheld.
 */
#ifndef CONFIG_TX_POWER_LEVELS
#define CONFIG_TX_POWER_LEVELS 1000, 2500, 5000
#endif

static const uint32_t txPowerLevels[] = { CONFIG_TX_POWER_LEVELS };

const uint16_t ctcss_tone[CTCSS_FREQ_NUM] =
{
    670, 693, 719, 744, 770, 797, 825, 854, 885, 915, 948, 974, 1000, 1035,
    1072, 1109, 1148, 1188, 1230, 1273, 1318, 1365, 1413, 1462, 1514, 1567,
    1598, 1622, 1655, 1679, 1713, 1738, 1773, 1799, 1835, 1862, 1899, 1928,
    1966, 1995, 2035, 2065, 2107, 2181, 2257, 2291, 2336, 2418, 2503, 2541
};

channel_t cps_getDefaultChannel()
{
    channel_t channel;

    #ifdef PLATFORM_MOD17
    channel.mode      = OPMODE_M17;
    #else
    channel.mode      = OPMODE_FM;
    #endif
    channel.bandwidth = BW_25;
    channel.power     = txPowerLevels[0];   // Lowest power level
    channel.rx_only   = false;  // Enable tx by default

    // Set initial frequency based on supported bands
    const hwInfo_t* hwinfo  = platform_getHwInfo();
    if(hwinfo->uhf_band)
    {
        channel.rx_frequency = 430000000;
        channel.tx_frequency = 430000000;
    }
    else if(hwinfo->vhf_band)
    {
        channel.rx_frequency = 144000000;
        channel.tx_frequency = 144000000;
    }

    channel.fm.rxToneEn = 0; //disabled
    channel.fm.rxTone   = 0; //and no ctcss/dcs selected
    channel.fm.txToneEn = 0;
    channel.fm.txTone   = 0;
    return channel;
}

uint32_t cps_nextTxPower(const uint32_t power)
{
    for(size_t i = 0; i < ARRAY_SIZE(txPowerLevels); i++)
    {
        if(txPowerLevels[i] > power)
            return txPowerLevels[i];
    }

    return txPowerLevels[0];
}
