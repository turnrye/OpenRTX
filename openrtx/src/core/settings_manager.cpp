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
constexpr size_t SLICE_SIZE = 8;
using slice_t = std::array<uint8_t, SLICE_SIZE>;
constexpr size_t REQUIRED_SLICE_COUNT =
    (sizeof(settings_t) + SLICE_SIZE - 1) / SLICE_SIZE;
using raw_settings_t = std::array<uint8_t, SLICE_SIZE * REQUIRED_SLICE_COUNT>;

constexpr uint8_t SETTINGS_T_MAGIC = 211;
constexpr uint8_t SETTINGS_T_STORED_BYTE_LENGTH =
    static_cast<uint8_t>(sizeof(settings_t));

slice_t sliceFromSettings(const settings_t &settings, size_t sliceIndex)
{
    slice_t out;
    out.fill(DEFAULT_SETTINGS_VALUE);

    auto sbytes = reinterpret_cast<const uint8_t *>(&settings);
    auto dbytes = reinterpret_cast<const uint8_t *>(&default_settings);
    size_t offset = sliceIndex * SLICE_SIZE;
    size_t total = sizeof(settings_t);

    for (size_t i = 0; i < SLICE_SIZE; ++i) {
        size_t gi = offset + i;
        if (sliceIndex == 0 && i < 2) {
            out[i] = (i == 0) ? SETTINGS_T_MAGIC :
                                SETTINGS_T_STORED_BYTE_LENGTH;
        } else if (gi < total && sbytes[gi] != dbytes[gi]) {
            out[i] = sbytes[gi];
        }
    }
    return out;
}

void loadSettingsFromSlice(settings_t &settings, const slice_t &raw,
                           size_t sliceIndex)
{
    auto sbytes = reinterpret_cast<uint8_t *>(&settings);
    auto dbytes = reinterpret_cast<const uint8_t *>(&default_settings);
    size_t offset = sliceIndex * SLICE_SIZE;
    size_t total = sizeof(settings_t);
    size_t end = std::min(offset + SLICE_SIZE, total);

    for (size_t gi = offset; gi < end; ++gi) {
        size_t i = gi - offset;
        if (sliceIndex == 0 && i < 2) {
            sbytes[gi] = raw[i];
        } else {
            sbytes[gi] = (raw[i] == DEFAULT_SETTINGS_VALUE) ? dbytes[gi] :
                                                              raw[i];
        }
    }
}
} // namespace

/**
 * Load settings from memory; this method handles loading defaults and initializing memory if necessary
 * 
 * @param settings_t: place to load settings in memory
 * @returns 0 if successful, <0 otherwise
 */
int settings_load(settings_t *out)
{
    if (!out)
        return -1;

    raw_settings_t r;
    int ret = nvm_readSettings(r.data(), r.size());
    if (ret < 0)
        return ret;

    size_t stored_bytes = (r[0] == SETTINGS_T_MAGIC && r[1] <= r.size()) ?
                              static_cast<size_t>(r[1]) :
                              0;

    size_t slices_present =
        stored_bytes ? ((stored_bytes + SLICE_SIZE - 1) / SLICE_SIZE) : 0;
    const size_t total_bytes = sizeof(settings_t);
    const size_t required_slices = (total_bytes + SLICE_SIZE - 1) / SLICE_SIZE;

    for (size_t i = slices_present; i < required_slices; ++i) {
        auto emptySlice = sliceFromSettings(default_settings, i);
        nvm_writeSettingsSlice(emptySlice.data(), emptySlice.size(),
                               i * emptySlice.size());
        std::copy_n(emptySlice.data(), SLICE_SIZE, r.data() + i * SLICE_SIZE);
    }

    for (size_t i = 0; i < required_slices; ++i) {
        slice_t slice;
        std::copy_n(r.data() + i * SLICE_SIZE, SLICE_SIZE, slice.data());
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

    const size_t total_bytes = sizeof(settings_t);
    const size_t required_slices = (total_bytes + SLICE_SIZE - 1) / SLICE_SIZE;

    for (size_t i = 0; i < required_slices; ++i) {
        auto slice = sliceFromSettings(*in, i);
        nvm_writeSettingsSlice(slice.data(), slice.size(), i * slice.size());
    }
    return 0;
}
