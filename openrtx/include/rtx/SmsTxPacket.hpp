/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SMSTXPACKET_H
#define SMSTXPACKET_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include "protocols/M17/PacketFrame.hpp"
#include <cstddef>
#include <cstdint>

/**
 * Assemble an SMS message into a sequence of M17 PacketFrames ready for
 * transmission.
 *
 * This is an application-layer helper that prepends protocol ID 0x05 and
 * appends a NUL terminator to the message text, then delegates to the
 * generic data link layer buildPacketFrames() for CRC computation and
 * 25-byte frame chunking.
 *
 * @param message:   Pointer to the NUL-terminated SMS text.
 * @param msgLen:    Length of the message NOT counting the NUL terminator
 *                   (i.e. the value returned by strlen()).
 * @param frames:    Output array of PacketFrames to fill.
 * @param maxFrames: Capacity of the frames array.
 * @return           Number of PacketFrames written (0 if msgLen is 0 or
 *                   the message is too large for the output array).
 */
size_t buildSmsPacketFrames(const char *message, size_t msgLen,
                            M17::PacketFrame *frames, size_t maxFrames);

#endif /* SMSTXPACKET_H */
