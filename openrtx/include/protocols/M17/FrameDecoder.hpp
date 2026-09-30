/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef FRAMEDECODER_H
#define FRAMEDECODER_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <cstdint>
#include <string>
#include <array>
#include "LinkSetupFrame.hpp"
#include "Viterbi.hpp"
#include "StreamFrame.hpp"
#include "PacketFrame.hpp"

namespace M17
{

enum class FrameType : uint8_t {
    PREAMBLE = 0,   ///< Frame contains a preamble.
    LINK_SETUP = 1, ///< Frame is a Link Setup Frame.
    STREAM = 2,     ///< Frame is a stream data frame.
    PACKET = 3,     ///< Frame is a packet data frame.
    EOT = 4,        ///< Frame is an End Of Transmission frame.
    UNKNOWN = 5     ///< Frame is unknown.
};

/**
 * M17 frame decoder.
 */
class FrameDecoder
{
public:
    /**
     * Constructor.
     */
    FrameDecoder();

    /**
     * Destructor.
     */
    ~FrameDecoder();

    /**
     * Clear the internal data structures.
     */
    void reset();

    /**
     * Decode an M17 frame, identifying its type. Frame data must contain the
     * sync word in the first two bytes. The payload is decoded with a
     * soft-decision Viterbi using the per-bit confidence values in @p soft,
     * which are parallel to the coded bits of @p frame: 0x0000 is a confident
     * 0, 0xFFFF a confident 1 (see Demodulator::getSoftFrame()).
     *
     * @param frame: byte array containing frame data.
     * @param soft: soft bits, one per coded bit of the frame.
     * @return the type of frame recognized.
     */
    FrameType decodeFrame(const frame_t &frame, const softFrame_t &soft);

    /**
     * Decode an M17 frame from hard bits only. Every bit is treated as fully
     * confident, which makes the decoder behave as a hard-decision one.
     *
     * @param frame: byte array containing frame data.
     * @return the type of frame recognized.
     */
    FrameType decodeFrame(const frame_t &frame);

    /**
     * Get the latest Link Setup Frame decoded. Check of the validity of the
     * data contained in the LSF is left to application code.
     *
     * @return a reference to the latest Link Setup Frame decoded.
     */
    const LinkSetupFrame &getLsf()
    {
        return lsf;
    }

    /**
     * Get the latest stream data frame decoded.
     *
     * @return a reference to the latest stream data frame decoded.
     */
    const StreamFrame &getStreamFrame()
    {
        return streamFrame;
    }

    /**
     * Get the latest packet data frame decoded.
     *
     * @return a reference to the latest packet data frame decoded.
     */
    const PacketFrame &getPacketFrame() const
    {
        return packetFrame;
    }

private:
    /**
     * Determine frame type by searching which syncword among the standard M17
     * ones has the minumum hamming distance from the given one. If the hamming
     * distance exceeds a masimum absolute threshold the frame is declared of
     * unknown type.
     *
     * @param syncWord: frame syncword.
     * @return frame type based on the given syncword.
     */
    FrameType getFrameType(const std::array<uint8_t, 2> &syncWord);

    /**
     * Decode Link Setup Frame data and update the internal LSF field with
     * the new frame data.
     *
     * @param soft: soft bits of the frame payload, decorrelated and
     * deinterleaved, without sync word.
     */
    void decodeLSF(const std::array<uint16_t, 368> &soft);

    /**
     * Decode stream data and update the internal LSF field with the new
     * frame data.
     *
     * @param soft: soft bits of the frame payload, decorrelated and
     * deinterleaved, without sync word.
     */
    void decodeStream(const std::array<uint16_t, 368> &soft);

    /**
     * Decode packet data and update the internal packet frame field with the
     * new frame data.
     *
     * @param soft: soft bits of the frame payload, decorrelated and
     * deinterleaved, without sync word.
     */
    void decodePacket(const std::array<uint16_t, 368> &soft);

    /**
     * Decode a LICH block.
     *
     * @param segment: byte array where to store the decoded Link Setup Frame
     * segment. The last byte contains the segment number.
     * @param lich: LICH block to be decoded.
     * @return true when the LICH block is successfully decoded.
     */
    bool decodeLich(std::array<uint8_t, 6> &segment, const lich_t &lich);

    uint8_t lsfSegmentMap;      ///< Bitmap for LSF reassembly from LICH
    LinkSetupFrame lsf;         ///< Latest LSF received.
    LinkSetupFrame lsfFromLich; ///< LSF assembled from LICH segments.
    StreamFrame streamFrame;    ///< Latest stream dat frame received.
    PacketFrame packetFrame;    ///< Latest packet data frame received.
    SoftViterbi viterbi;        ///< Soft-decision Viterbi decoder.

    // Scratch buffers are members rather than locals: a frame decode runs on
    // the RTX thread, whose stack is small.
    softFrame_t softFromHard; ///< Soft bits derived from hard input.
    std::array<uint16_t, 368>
        softPayload;          ///< Decorrelated, deinterleaved payload.
    std::array<uint16_t, 272> softStream; ///< Stream payload after the LICH.

    ///< Maximum allowed hamming distance when determining the frame type.
    static constexpr uint8_t MAX_SYNC_HAMM_DISTANCE = 4;

    /**
     * Maximum Viterbi cost, in full-scale soft units, for a stream payload to
     * be accepted; stream frames carry no CRC, so this is what keeps a badly
     * decoded frame away from the codec. With hard (saturated) input the cost
     * equals the number of corrected bit errors. With demodulator soft bits
     * the +-1 symbols carry a less confident sign bit, so even a clean frame
     * costs 25 to 45 units. Measured over the air on an MD-UV380: frames of
     * packets that passed their CRC never exceeded 60 (6200 frames), frames
     * that broke a CRC cost 59 to 75.
     */
    static constexpr uint16_t MAX_VITERBI_COST = 64;
};

} // namespace M17

#endif // FRAMEDECODER_H
