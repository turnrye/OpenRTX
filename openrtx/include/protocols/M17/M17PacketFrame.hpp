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
 * This class describes and handles an M17 packet data frame.
 * A packet frame contains 25 bytes of payload data and 6 bits of metadata.
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
    ~M17PacketFrame(){ }

    /**
     * Clear the frame content, filling it with zeroes.
     */
    void clear()
    {
        memset(&data, 0x00, sizeof(data));
    }

    /**
     * Set the End of Frame (EOF) bit.
     *
     * @param eof: true if this is the last packet frame in the transmission.
     */
    void setEofBit(const bool eof)
    {
        if(eof)
            data.metadata |= EOF_BIT;
        else
            data.metadata &= ~EOF_BIT;
    }

    /**
     * Get the End of Frame (EOF) bit.
     *
     * @return true if this is the last packet frame in the transmission.
     */
    bool getEofBit() const
    {
        return (data.metadata & EOF_BIT) != 0;
    }

    /**
     * Set the packet frame counter or byte counter.
     * If EOF bit is 0, this is the frame number (0-31).
     * If EOF bit is 1, this is the byte count in the last frame (1-25).
     *
     * @param counter: frame counter (0-31) or byte counter (1-25).
     */
    void setCounter(const uint8_t counter)
    {
        data.metadata = (data.metadata & ~COUNTER_MASK) | ((counter << 2) & COUNTER_MASK);
    }

    /**
     * Get the packet frame counter or byte counter.
     * If EOF bit is 0, this returns the frame number (0-31).
     * If EOF bit is 1, this returns the byte count in the last frame (1-25).
     *
     * @return frame counter (0-31) or byte counter (1-25).
     */
    uint8_t getCounter() const
    {
        return (data.metadata & COUNTER_MASK) >> 2;
    }

    /**
     * Access frame payload.
     *
     * @return a reference to frame's payload field, allowing for both read and
     * write access.
     */
    payload_t& payload()
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
        return reinterpret_cast < const uint8_t * > (&data);
    }

private:

    struct __attribute__((packed))
    {
        payload_t payload;   // 25 bytes of payload data
        uint8_t   metadata;  // 1 EOF bit + 5 counter bits + 2 reserved bits
    }
    data;

    static constexpr uint8_t EOF_BIT      = 0x80;  // EOF bit (bit 7)
    static constexpr uint8_t COUNTER_MASK = 0x7C;  // Counter bits (bits 2-6)

    // Frame decoder class needs to access raw frame data
    friend class M17FrameDecoder;
};

}      // namespace M17

#endif // M17_PACKET_FRAME_H
