/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rtx/SmsTxPacket.hpp"
#include "protocols/M17/PacketAssembler.hpp"
#include <cstring>

size_t buildSmsPacketFrames(const char *message, size_t msgLen,
                            M17::PacketFrame *frames, size_t maxFrames)
{
    if (msgLen == 0 || message == nullptr || frames == nullptr
        || maxFrames == 0)
        return 0;

    // SMS application format: [0x05][UTF-8 text][0x00]
    const size_t appDataLen = 1 + msgLen + 1;

    if (appDataLen + 2 > M17::MAX_PACKET_DATA)
        return 0;

    uint8_t appData[M17::MAX_PACKET_DATA];
    appData[0] = 0x05;
    memcpy(&appData[1], message, msgLen);
    appData[1 + msgLen] = 0x00;

    return M17::buildPacketFrames(appData, appDataLen, frames, maxFrames);
}
