/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef METATEXT_H
#define METATEXT_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <cstdint>
#include <string>
#include <array>
#include "M17LinkSetupFrame.hpp"
#include "M17Viterbi.hpp"
#include "M17StreamFrame.hpp"
#include <stdint.h>

namespace M17
{

class MetaText
{
public:
    /**
     * Constructor.
     */
    MetaText();

    /**
     * Constructor.
     */
    MetaText(const char *text);

    /**
     * Destructor.
     */
    ~MetaText();

    /**
     * Add a new block.
     */
    bool addBlock(const meta_t &meta);

    /**
     * Get a specific block. If the block number is out of range, return
     * the first block.
     */
    const meta_t &getBlock(const uint8_t num) const
    {
        if (num >= MAX_BLOCKS)
            return blocks[0];
        return blocks[num];
    };

    /**
     * Return the assembled meta text, or an empty string if not all blocks
     * have been received yet.
     */
    const char *get();

    void reset();

    /**
 * Convert the M17 block index field to a normal integer
 * @param meta_t: the meta object
 * @returns the index for the current block (0 based counting)
*/
    static uint8_t getBlockIndex(const meta_t &meta)
    {
        uint8_t blockIndexMask = (meta.raw_data[0] & 0x0f);
        return __builtin_ctz(blockIndexMask);
    }

    /**
 * Convert the M17 total blocks field to a normal integer
 * @param meta_t: the meta object
 * @returns the number of total blocks to make up the MetaText
 */
    static uint8_t getTotalBlocks(const meta_t &meta)
    {
        uint8_t requiredBlocksMask = (meta.raw_data[0] & 0xf0) >> 4;
        if (requiredBlocksMask == 0x1)
            return 1;
        else if (requiredBlocksMask == 0x3)
            return 2;
        else if (requiredBlocksMask == 0x7)
            return 3;
        else if (requiredBlocksMask == 0xF)
            return 4;
        else
            return 0;
    }

private:
    constexpr static uint8_t MAX_BLOCKS = 4;
    constexpr static uint8_t BLOCK_LENGTH = 13;
    constexpr static uint8_t MAX_TEXT_LENGTH = MAX_BLOCKS * BLOCK_LENGTH;
    std::array<meta_t, MAX_BLOCKS> blocks;
    char metaTextBuffer[MAX_TEXT_LENGTH + 1];
};

} // namespace M17

#endif // METATEXT_H
