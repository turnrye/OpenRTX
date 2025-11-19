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

#include "core/settings.h"

#ifndef SETTINGS_MANAGER_HPP
#define SETTINGS_MANAGER_HPP

#ifdef __cplusplus

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
using settings_nvmem_segment_t =
    std::array<uint8_t, sizeof(slice_t) * REQUIRED_SLICE_COUNT>;

class NvmSliceManager
{
public:
    NvmSliceManager()
    {
    }

    ~NvmSliceManager()
    {
    }

    /**
     * Load settings from memory; if the nvmem is uninitialized (partially or fully), default settings will be loaded in memory instead
     * 
     * @param settings_t: place to load settings in memory
     * @returns 0 if successful, <0 otherwise
     */
    int load(settings_t *out);

    /**
     * Save the settings object to memory, observing default value selections and writing in memory slices.
     * 
     * @param settings_t: the settings to be written to memory
     * @returns 0 if successful, <0 otherwise
     */
    int save(const settings_t *in);

private:
    /**
     * @brief Read a specific slice from memory
     */
    slice_t readSlice(settings_nvmem_segment_t memory, size_t sliceIndex);
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
                               size_t sliceIndex);
    /**
     * @brief Construct the slice_t for a given index of settings; this is useful for creating blocks to be written to nvmem.
     * 
     * This method handles:
     * - Replacing defaults with DEFAULT
     * - Populating the checksum
     */
    slice_t sliceFromSettings(const settings_t &settings, size_t sliceIndex);
    /**
     * @brief Calculate the crc8 checksum for data
     */
    uint8_t crc8(const void *data, const size_t len);
};
/** C wrappers for compatibility with existing code */
extern "C" {
#endif

/**
 * This method is responsible for loading settings; it interacts with nvmem and handles settings-specific behaviors.
 * 
 * @param settings_t: pointer to the settings object to have settings read into
 * @returns 0 if sucessful, <0 if unsuccessful
 */
int settings_load(settings_t *out);

/**
 * Save a given settings object to memory
 * 
 * @param settings_t: pointer to the settings object to be saved
 * @returns 0 if successful, <0 if unsuccessful
 * 
 */
int settings_save(const settings_t *in);

#ifdef __cplusplus
}
#endif

#endif /* SETTIGNS_MANAGER_HPP*/
