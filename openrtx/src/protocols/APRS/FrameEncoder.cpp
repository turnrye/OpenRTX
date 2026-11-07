/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/APRS/FrameEncoder.hpp"
#include "core/crc.h"

#include <climits>

using namespace APRS;

FrameEncoder::FrameEncoder() :
    frame(nullptr), frameLen(0), byteIndex(0), flagsLeft(0), tailFlags(0),
    fcs{}, section(Section::DONE), bitIndex(0), onesRun(0), level(1)
{
}

FrameEncoder::~FrameEncoder()
{
}

void FrameEncoder::start(const uint8_t *frame, size_t len, size_t leadFlags,
                         size_t tailFlags)
{
    const uint16_t crc = crc_hdlc(frame, len);

    this->frame = frame;
    this->tailFlags = tailFlags;
    frameLen = len;
    fcs[0] = (uint8_t)(crc & 0xff);
    fcs[1] = (uint8_t)(crc >> 8);

    section = Section::LEAD;
    flagsLeft = leadFlags;
    byteIndex = 0;
    bitIndex = 0;
    onesRun = 0;
    level = 1;
}

bool FrameEncoder::nextSymbol(uint8_t &tone)
{
    uint8_t bit;

    if (!nextBit(bit))
        return false;

    // NRZI: a zero changes the tone, a one keeps it
    if (bit == 0)
        level ^= 1;

    tone = level;
    return true;
}

bool FrameEncoder::nextBit(uint8_t &bit)
{
    switch (section) {
        case Section::LEAD:
            if (flagsLeft > 0) {
                bit = flagBit();
                return true;
            }

            section = Section::DATA;
            bitIndex = 0;
            onesRun = 0;
            // fallthrough

        case Section::DATA:
            // A stuffed zero after the last five ones still belongs to the
            // data, before the closing flag
            if ((byteIndex < (frameLen + APRS_FCS_LEN))
                || (onesRun == MAX_ONES)) {
                bit = dataBit();
                return true;
            }

            section = Section::TAIL;
            flagsLeft = tailFlags;
            bitIndex = 0;
            // fallthrough

        case Section::TAIL:
            if (flagsLeft > 0) {
                bit = flagBit();
                return true;
            }

            section = Section::DONE;
            // fallthrough

        case Section::DONE:
            break;
    }

    return false;
}

uint8_t FrameEncoder::flagBit()
{
    const uint8_t bit = (HDLC_FLAG >> bitIndex) & 1;

    bitIndex++;
    if (bitIndex == CHAR_BIT) {
        bitIndex = 0;
        flagsLeft--;
    }

    return bit;
}

uint8_t FrameEncoder::dataBit()
{
    if (onesRun == MAX_ONES) {
        onesRun = 0;
        return 0;
    }

    const uint8_t byte = (byteIndex < frameLen) ? frame[byteIndex] :
                                                  fcs[byteIndex - frameLen];
    // AX.25 sends the least significant bit first
    const uint8_t bit = (byte >> bitIndex) & 1;

    bitIndex++;
    if (bitIndex == CHAR_BIT) {
        bitIndex = 0;
        byteIndex++;
    }

    onesRun = (bit != 0) ? (onesRun + 1) : 0;
    return bit;
}
