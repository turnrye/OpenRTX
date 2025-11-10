/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/M17/M17Golay.hpp"
#include "protocols/M17/MetaText.hpp"
#include "protocols/M17/M17Interleaver.hpp"
#include "protocols/M17/M17Decorrelator.hpp"
#include "protocols/M17/M17CodePuncturing.hpp"
#include "protocols/M17/M17Constants.hpp"
#include "protocols/M17/M17Utils.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdint>

using namespace M17;

MetaText::MetaText()
{
}

MetaText::~MetaText()
{
}

void MetaText::reset()
{
    blocks = {};
    metaTextBuffer[0] = '\0';
}

bool MetaText::addBlock(const meta_t &meta)
{
    uint8_t blockIndex = getBlockIndex(meta);

    if (blockIndex < MAX_BLOCKS) {
        blocks[blockIndex] = meta;
        return true;
    }
    return false;
}
const char *MetaText::get()
{
    memset(metaTextBuffer, 0, sizeof(metaTextBuffer));

    // Stitch the blocks together
    for (size_t i = 0; i < blocks.size(); i++) {
        const auto &meta = blocks[i];
        if (meta.raw_data[0] == 0 || i >= getTotalBlocks(meta))
            break;

        memcpy(metaTextBuffer + (i * BLOCK_LENGTH), meta.raw_data + 1,
               BLOCK_LENGTH);
    }

    // Set the string terminator after the last non-space character
    size_t lastNonStringPos = SIZE_MAX;
    for (size_t i = 0; i < MAX_TEXT_LENGTH; ++i) {
        if (metaTextBuffer[i] != ' ') {
            lastNonStringPos = i;
        }
    }
    if (lastNonStringPos != SIZE_MAX) {
        metaTextBuffer[lastNonStringPos + 1] = '\0';
    } else {
        metaTextBuffer[0] = '\0';
    }
    return metaTextBuffer;
}
