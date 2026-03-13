/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef M17_PACKET_FRAME_H
#define M17_PACKET_FRAME_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <cstring>
#include <string>
#include "M17Datatypes.hpp"

namespace M17
{

class M17FrameDecoder;

/**
 * This class describes and handles a generic M17 packet frame.
 */
class M17PacketFrame
{
public:
    /**
     * Constructor.
     */
    M17PacketFrame()
    {
        clear();
    }

    /**
     * Destructor.
     */
    ~M17PacketFrame()
    {
    }

    /**
     * Clear the frame content, filling it with zeroes.
     */
    void clear()
    {
        memset(&data, 0x00, sizeof(data));
    }

    /**
     * Access frame payload.
     *
     * @return a reference to frame's payload field, allowing for both read and
     * write access.
     */
    pktPayload_t &payload()
    {
        return data.payload;
    }

    /**
     * Access frame payload (const overload).
     *
     * @return a const reference to frame's payload field.
     */
    const pktPayload_t &payload() const
    {
        return data.payload;
    }

    /**
     * Get underlying data.
     *
     * @return a pointer to const uint8_t allowing direct access to frame data.
     */
    const uint8_t *getData() const
    {
        return reinterpret_cast<const uint8_t *>(&data);
    }

private:
    struct __attribute__((packed)) {
        pktPayload_t payload; // Payload data
    } data;
    ///< Frame data.
    // Frame decoder class needs to access raw frame data
    friend class M17FrameDecoder;
};

} // namespace M17

#endif // M17_PACKET_FRAME_H
