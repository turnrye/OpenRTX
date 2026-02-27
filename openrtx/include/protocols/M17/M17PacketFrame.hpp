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

#include "M17Datatypes.hpp"

namespace M17
{

/**
 * This class describes and handles a generic M17 packet frame.
 */
class M17PacketFrame
{
    static constexpr size_t META_INDEX = 25;
    static constexpr uint8_t EOF_BIT = 0x80;
    static constexpr uint8_t COUNTER_MASK = 0x1F;
    static constexpr uint8_t COUNTER_SHIFT = 2;
    static constexpr uint8_t COUNTER_CLEAR =
        static_cast<uint8_t>(~(COUNTER_MASK << COUNTER_SHIFT));

public:
    /**
     * Constructor.
     */
    M17PacketFrame()
    {
        clear();
    }

    /**
     * Clear the frame content, filling it with zeroes.
     */
    void clear()
    {
        data.fill(0);
    }

    /**
     * Access frame payload.
     *
     * @return a reference to frame's payload field, allowing for both read and
     * write access.
     */
    pktPayload_t &payload()
    {
        return data;
    }

    /**
     * Access frame payload (const overload).
     *
     * @return a const reference to frame's payload field.
     */
    const pktPayload_t &payload() const
    {
        return data;
    }

    /**
     * Set the EOF (last frame) flag.
     *
     * @param eof: true to mark this as the last packet frame.
     */
    void setEof(bool eof)
    {
        if (eof)
            data[META_INDEX] |= EOF_BIT;
        else
            data[META_INDEX] &= ~EOF_BIT;
    }

    /**
     * Check if this is the last packet frame.
     *
     * @return true if the EOF bit is set.
     */
    bool isEof() const
    {
        return (data[META_INDEX] & EOF_BIT) != 0;
    }

    /**
     * Set the 5-bit frame/byte counter (bits 6..2 of byte 25).
     *
     * @param counter: value between 0 and 31.
     */
    void setCounter(uint8_t counter)
    {
        data[META_INDEX] = (data[META_INDEX] & COUNTER_CLEAR)
                         | ((counter & COUNTER_MASK) << COUNTER_SHIFT);
    }

    /**
     * Get the 5-bit frame/byte counter.
     *
     * @return counter value between 0 and 31.
     */
    uint8_t getCounter() const
    {
        return (data[META_INDEX] >> COUNTER_SHIFT) & COUNTER_MASK;
    }

    /**
     * Get underlying data.
     *
     * @return a pointer to const uint8_t allowing direct access to frame data.
     */
    const uint8_t *getData() const
    {
        return data.data();
    }

private:
    pktPayload_t data;
};

} // namespace M17

#endif // M17_PACKET_FRAME_H
