/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APRS_FRAMEENCODER_H
#define APRS_FRAMEENCODER_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include "protocols/APRS/constants.h"

namespace APRS
{

/**
 * HDLC framing for transmit, the inverse of FrameDecoder: opening flags, the
 * frame and its FCS with bit stuffing, closing flags, all NRZI encoded into
 * the tone levels the modulator sends. Symbols are produced one at a time, so
 * a caller can generate as much baseband as it has room for.
 */
class FrameEncoder
{
public:
    /**
     * Constructor.
     */
    FrameEncoder();

    /**
     * Destructor.
     */
    ~FrameEncoder();

    /**
     * Start encoding a frame. The frame is not copied and must stay valid
     * until nextSymbol() returns false.
     *
     * @param frame: AX.25 frame, without its FCS.
     * @param len: frame length in bytes.
     * @param leadFlags: flags sent before the frame, at least one.
     * @param tailFlags: flags sent after the frame, at least one.
     */
    void start(const uint8_t *frame, size_t len, size_t leadFlags,
               size_t tailFlags);

    /**
     * Produce the next symbol.
     *
     * @param tone: set to the tone to send, 1 for mark and 0 for space.
     * @return true if a symbol was produced, false once the last closing flag
     *         has been sent.
     */
    bool nextSymbol(uint8_t &tone);

private:
    /**
     * Produce the next HDLC bit, before NRZI encoding.
     *
     * @param bit: set to the bit to send.
     * @return true if a bit was produced, false at the end of the frame.
     */
    bool nextBit(uint8_t &bit);

    /**
     * Next bit of the flags being sent, without stuffing.
     */
    uint8_t flagBit();

    /**
     * Next bit of the frame and FCS, with a zero stuffed after five ones.
     */
    uint8_t dataBit();

    /**
     * Part of the transmission being encoded.
     */
    enum class Section {
        LEAD, ///< Opening flags.
        DATA, ///< Frame and FCS.
        TAIL, ///< Closing flags.
        DONE, ///< Nothing left to send.
    };

    static constexpr uint8_t HDLC_FLAG = 0x7e; ///< Flag byte.
    static constexpr uint8_t MAX_ONES = 5;     ///< Ones before a stuffed 0.

    const uint8_t *frame;                      ///< Frame being sent.
    size_t frameLen;                           ///< Frame length.
    size_t byteIndex;                          ///< Byte of frame and FCS.
    size_t flagsLeft;                          ///< Flags left in this section.
    size_t tailFlags;                          ///< Closing flags to send.
    std::array<uint8_t, APRS_FCS_LEN> fcs;     ///< FCS, low byte first.
    Section section;                           ///< Part being sent.
    uint8_t bitIndex;                          ///< Bit of the current byte.
    uint8_t onesRun;                           ///< Ones sent in a row.
    uint8_t level;                             ///< NRZI level: 1 mark, 0 space.
};

} /* APRS */

#endif /* APRS_FRAMEENCODER_H */
