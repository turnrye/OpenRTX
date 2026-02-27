/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef M17_PACKETASSEMBLER_H
#define M17_PACKETASSEMBLER_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include "PacketFrame.hpp"
#include <cstddef>
#include <cstdint>

namespace M17
{

/**
 * Maximum number of bytes in an M17 Single Packet (application data + CRC).
 * Per the spec, up to 823 bytes of application data plus 2 bytes of CRC,
 * yielding at most 33 frames of 25 bytes each.
 */
static constexpr size_t MAX_PACKET_DATA = 33 * 25;

/**
 * Take raw application packet data, compute and append the M17 CRC, then
 * split into 25-byte PacketFrame chunks with correct counter and EOF metadata
 * per the M17 specification Data Link Layer Packet Mode.
 *
 * @param packetData: Application packet bytes (protocol ID + payload).
 *                    The CRC is computed and appended by this function.
 * @param dataLen:    Length of packetData in bytes (max 823).
 * @param frames:     Output array of PacketFrames to fill.
 * @param maxFrames:  Capacity of the frames array.
 * @return            Number of PacketFrames written (0 on error).
 */
size_t buildPacketFrames(const uint8_t *packetData, size_t dataLen,
                         PacketFrame *frames, size_t maxFrames);

} // namespace M17

#endif /* M17_PACKETASSEMBLER_H */
