/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/M17/PacketAssembler.hpp"
#include "core/crc.h"
#include <cstring>

namespace M17
{

size_t buildPacketFrames(const uint8_t *packetData, size_t dataLen,
                         PacketFrame *frames, size_t maxFrames)
{
    if (dataLen == 0 || packetData == nullptr || frames == nullptr
        || maxFrames == 0)
        return 0;

    const size_t totalLen = dataLen + 2; // + 2-byte CRC

    if (totalLen > MAX_PACKET_DATA)
        return 0;

    uint8_t packetBuffer[MAX_PACKET_DATA];
    memset(packetBuffer, 0, sizeof(packetBuffer));
    memcpy(packetBuffer, packetData, dataLen);

    // CRC is computed over application data, then stored big-endian.
    uint16_t crc = crc_m17(packetBuffer, dataLen);
    packetBuffer[dataLen] = static_cast<uint8_t>(crc >> 8);
    packetBuffer[dataLen + 1] = static_cast<uint8_t>(crc & 0xFF);

    // Compute number of frames needed
    const size_t numFrames = (totalLen + PacketFrame::DATA_SIZE - 1)
                           / PacketFrame::DATA_SIZE;
    if (numFrames > maxFrames)
        return 0;

    size_t remaining = totalLen;
    size_t offset = 0;

    for (size_t i = 0; i < numFrames; i++) {
        frames[i].clear();

        if (remaining > PacketFrame::DATA_SIZE) {
            // Intermediate frame
            memcpy(frames[i].data(), &packetBuffer[offset],
                   PacketFrame::DATA_SIZE);
            frames[i].setCounter(static_cast<uint8_t>(i));
            offset += PacketFrame::DATA_SIZE;
            remaining -= PacketFrame::DATA_SIZE;
        } else {
            // Final frame
            memcpy(frames[i].data(), &packetBuffer[offset], remaining);
            frames[i].setEof(true);
            frames[i].setCounter(static_cast<uint8_t>(remaining));
        }
    }

    return numFrames;
}

} // namespace M17
