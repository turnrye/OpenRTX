/***************************************************************************
 *   Copyright (C)        2025 by Federico Amedeo Izzo IU2NUO,             *
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

#include <cstddef>
#include <cstdint>
#include <array>
#include <algorithm>
#include <stdio.h>
#include "core/settings_manager.hpp"
#include "interfaces/nvmem.h"

namespace
{
constexpr uint8_t DEFAULT_SETTINGS_VALUE = 0xFF;
/**
 * @brief The number of bytes that are contained within a slice message
 */
constexpr size_t SLICE_MESSAGE_BYTES = 8;
/**
 * @brief The slice message is a portion of the settings_t struct, except defaults are replaced with the DEFAULT_SETTINGS_VALUE
 */
using slice_message_t = std::array<uint8_t, SLICE_MESSAGE_BYTES>;

/**
 * @brief The number of slices required in order to persist all of the settings_t struct
 */
constexpr size_t REQUIRED_SLICE_COUNT =
    (sizeof(settings_t) + SLICE_MESSAGE_BYTES - 1) / SLICE_MESSAGE_BYTES;

/**
 * @brief The slice_t struct is what is ultimately read and written to nvmem
 */
typedef struct {
    slice_message_t message;
    uint8_t checksum;
} __attribute__((packed)) slice_t;

/**
 * @brief The raw memory segment that contains the slice_t's
 */
using settings_nvmem_segment_t = std::array<uint8_t, sizeof(slice_t) * REQUIRED_SLICE_COUNT>;


/**
 * @brief Calculate the crc8 checksum for data
 */
uint8_t crc8(const void *data, const size_t len)
{
    const uint8_t *ptr = reinterpret_cast< const uint8_t *>(data);
    uint8_t crc = 0xff;
    
    for(size_t i = 0; i < len; i++)
    {
        crc ^= ptr[i];
        
        for(uint8_t j = 0; j < 8; j++)
        {
            if(crc & 0x80)
                crc = (crc << 1) ^ 0x31;
            else
                crc = (crc << 1);
        }
    }
    return crc;
}

/**
 * @brief Construct the slice_t for a given index of settings; this is useful for creating blocks to be written to nvmem.
 * 
 * This method handles:
 * - Replacing defaults with DEFAULT
 * - Populating the checksum
 */
slice_t sliceFromSettings(const settings_t &settings, size_t sliceIndex)
{
    slice_t slice;
    slice.message.fill(DEFAULT_SETTINGS_VALUE);

    auto sbytes = reinterpret_cast<const uint8_t *>(&settings);
    auto dbytes = reinterpret_cast<const uint8_t *>(&default_settings);
    size_t offset = sliceIndex * SLICE_MESSAGE_BYTES;
    size_t total = sizeof(settings_t);

    for (size_t i = 0; i < SLICE_MESSAGE_BYTES; ++i) {
        size_t gi = offset + i;
        if (gi < total && sbytes[gi] != dbytes[gi]) {
            slice.message[i] = sbytes[gi];
        }
    }
    slice.checksum = crc8(slice.message.data(), slice.message.size());
    return slice;
}

/**
 * @brief Populate settings with a given slice and its index; this is useful for reassembling blocks after reading from nvmem.
 * 
 * This method handles:
 * - Loading defaults
 * - Checking the checksum
 * 
 * @returns true if the operation was completed, false if the checksum failed indicating a corrupt/uninitialized slice in nvmem (in which case settings is loaded with defaults)
 */
bool loadSettingsFromSlice(settings_t &settings, const slice_t &slice,
                           size_t sliceIndex)
{
    auto sbytes = reinterpret_cast<uint8_t *>(&settings);
    auto dbytes = reinterpret_cast<const uint8_t *>(&default_settings);
    size_t offset = sliceIndex * SLICE_MESSAGE_BYTES;
    size_t total = sizeof(settings_t);
    size_t end = std::min(offset + SLICE_MESSAGE_BYTES, total);
    bool checksumFailed = false;
    if(slice.checksum != crc8(slice.message.data(), slice.message.size())) {
        checksumFailed = true;
    }
    for (size_t gi = offset; gi < end; ++gi) {
        size_t i = gi - offset;
        sbytes[gi] = (slice.message[i] == DEFAULT_SETTINGS_VALUE || checksumFailed) ? dbytes[gi] :
                                                            slice.message[i];
    }
    return !checksumFailed;
}

/**
 * @brief Read a specific slice from memory
 */
slice_t readSlice(settings_nvmem_segment_t memory, size_t sliceIndex) {
    slice_t slice;
    std::copy_n(memory.data() + sliceIndex * sizeof(slice_t), sizeof(slice_t), reinterpret_cast<uint8_t*>(&slice));
    return slice;
}
} // namespace

/**
 * Load settings from memory; if the nvmem is uninitialized (partially or fully), default settings will be loaded in memory instead
 * 
 * @param settings_t: place to load settings in memory
 * @returns 0 if successful, <0 otherwise
 */
int settings_load(settings_t *out)
{
    if (!out)
        return -1;

    settings_nvmem_segment_t r;
    int ret = nvm_readSettings(r.data(), r.size());
    if (ret < 0)
        return ret;

    for (size_t i = 0; i < REQUIRED_SLICE_COUNT; ++i) {
        slice_t slice = readSlice(r, i);
        loadSettingsFromSlice(*out, slice, i);
    }

    return 0;
}

/**
 * Save the settings object to memory, observing default value selections and writing in memory slices.
 * 
 * @param settings_t: the settings to be written to memory
 * @returns 0 if successful, <0 otherwise
 */
int settings_save(const settings_t *in)
{
    if (!in)
        return -1;

    // Read memory first so that we can compare crcs for slices
    settings_nvmem_segment_t nvmem;
    int ret = nvm_readSettings(nvmem.data(), nvmem.size());
    if (ret < 0)
        return ret;

    for (size_t i = 0; i < REQUIRED_SLICE_COUNT; ++i) {
        // Read slice from nvmem and compare checksums; only write if the checksum is different (meaning the settings are actually different)
        auto slice = sliceFromSettings(*in, i);
        if(readSlice(nvmem, i).checksum != slice.checksum) {
            auto ret = nvm_writeSettingsSlice(reinterpret_cast<uint8_t*>(&slice), sizeof(slice_t), i * sizeof(slice_t));
            if (ret < 0)
                return ret;
        }
    }
    return 0;
}
