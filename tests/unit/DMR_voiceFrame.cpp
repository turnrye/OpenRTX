/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>
#include "protocols/DMR/VoiceFrame.h"

using Frame = std::vector<uint8_t>;

/*
 * Voice frames received over the air: from an MD-UV390 (vocoder in the radio)
 * and from a CS7000 Plus running OpenRTX, both captured with an SDR. Each was
 * checked clean on its first codeword by an independent decoder.
 */
static const std::vector<Frame> GOOD_FRAMES = {
    { 0xB9, 0xCE, 0xA3, 0x76, 0x8F, 0xBB, 0x69, 0xCA, 0xCD },
    { 0xDF, 0x88, 0x82, 0x76, 0x86, 0x84, 0x5E, 0x6F, 0xC1 },
    { 0xCC, 0x88, 0xA0, 0x54, 0xC0, 0xC5, 0x3C, 0x6F, 0x81 },
    { 0xDF, 0x8B, 0x82, 0x30, 0xA6, 0xA0, 0x0E, 0x4E, 0xE3 },
    { 0x23, 0x21, 0xD0, 0xE6, 0x02, 0x8B, 0x29, 0x36, 0x98 },
    { 0x02, 0x11, 0xF0, 0x80, 0x25, 0x8F, 0x58, 0x76, 0xCD },
    { 0xC0, 0xBF, 0xE2, 0x56, 0x00, 0xA9, 0x0C, 0xA6, 0xD1 },
    { 0x81, 0xFE, 0x6C, 0xE9, 0x24, 0xEB, 0xE8, 0x3F, 0x4D },
    { 0x90, 0xDF, 0x6E, 0xC8, 0x14, 0xCC, 0xB8, 0x2F, 0x1E },
    { 0x9D, 0xCB, 0xA6, 0x34, 0xEE, 0x01, 0x1D, 0xD4, 0x71 },
    { 0xBD, 0xEB, 0x87, 0x36, 0xEC, 0x32, 0x1D, 0xD1, 0x71 },
    { 0xEA, 0xD2, 0xA4, 0x32, 0x22, 0x8F, 0x5E, 0x8F, 0x75 },
    { 0xF6, 0xE1, 0x85, 0x46, 0x54, 0xC1, 0xD1, 0x4C, 0xCF },
    { 0x42, 0x71, 0xB0, 0x28, 0x00, 0xCF, 0x65, 0x59, 0x7E },
    { 0x49, 0xD8, 0xA0, 0x35, 0x11, 0xFF, 0xD9, 0x65, 0x89 },
    { 0xFB, 0xE2, 0xC6, 0x37, 0x48, 0x05, 0xE9, 0x14, 0x6F },
    { 0xA8, 0xFE, 0xA3, 0x72, 0xCC, 0x9F, 0x08, 0xEA, 0xCD },
    { 0xE2, 0xE4, 0xE1, 0x43, 0x39, 0x86, 0x70, 0xAD, 0x0F },
    { 0xA6, 0xC9, 0xE0, 0x74, 0x0A, 0x85, 0x2C, 0x67, 0x8A },
    { 0xDF, 0x46, 0xA4, 0x43, 0x5B, 0xE2, 0xA1, 0x95, 0x76 },
    { 0xF1, 0xC6, 0xC3, 0x06, 0x4F, 0xD4, 0x50, 0xE8, 0x4A },
    { 0xEF, 0x89, 0xC3, 0x16, 0xAD, 0x68, 0x9D, 0xF1, 0x89 },
    { 0xE4, 0xF2, 0x23, 0x5D, 0x1E, 0x9A, 0xC4, 0xFD, 0x47 },
    { 0xFA, 0x50, 0xA0, 0x24, 0x0B, 0x79, 0xC0, 0x8C, 0x88 },
    { 0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B }, // AMBE silence
};

static Frame flip(const Frame &f, int bit)
{
    Frame out = f;
    out[bit / 8] ^= 0x80 >> (bit % 8);
    return out;
}

TEST_CASE("DMR voice frames received over the air are error free",
          "[dmr][voice]")
{
    for (const auto &f : GOOD_FRAMES) {
        REQUIRE(dmr_voiceFrameErrors(f.data()) == 0);
        REQUIRE(dmr_isVoiceFrame(f.data()));
    }
}

TEST_CASE("DMR voice frames tolerate any single bit error", "[dmr][voice]")
{
    for (const auto &f : GOOD_FRAMES) {
        for (int bit = 0; bit < 72; bit++) {
            Frame bad = flip(f, bit);
            INFO("bit " << bit);
            REQUIRE(dmr_voiceFrameErrors(bad.data()) <= 1);
            REQUIRE(dmr_isVoiceFrame(bad.data()));
        }
    }
}

TEST_CASE("DMR voice frame codeword 0 is 23 of the 72 bits", "[dmr][voice]")
{
    /*
     * A misplaced entry in the de-interleave tables moves a bit in or out of
     * the codeword, which changes this count for some frame.
     */
    for (const auto &f : GOOD_FRAMES) {
        int inCodeword = 0;
        for (int bit = 0; bit < 72; bit++) {
            Frame bad = flip(f, bit);
            if (dmr_voiceFrameErrors(bad.data()) == 1)
                inCodeword++;
        }

        REQUIRE(inCodeword == 23);
    }
}

TEST_CASE("DMR voice frames with two errors in codeword 0 are rejected",
          "[dmr][voice]")
{
    const Frame &f = GOOD_FRAMES[0];

    std::vector<int> codewordBits;
    for (int bit = 0; bit < 72; bit++) {
        Frame bad = flip(f, bit);
        if (dmr_voiceFrameErrors(bad.data()) == 1)
            codewordBits.push_back(bit);
    }

    for (size_t i = 0; i < codewordBits.size(); i++) {
        for (size_t j = i + 1; j < codewordBits.size(); j++) {
            Frame bad = flip(flip(f, codewordBits[i]), codewordBits[j]);
            INFO("bits " << codewordBits[i] << " and " << codewordBits[j]);
            REQUIRE(dmr_voiceFrameErrors(bad.data()) >= 2);
            REQUIRE_FALSE(dmr_isVoiceFrame(bad.data()));
        }
    }
}

TEST_CASE("DMR idle payload is not voice", "[dmr][voice]")
{
    // All ones is what the HR_C6000 hands over with nothing received
    Frame ones(DMR_VOICE_FRAME_LENGTH, 0xFF);
    REQUIRE_FALSE(dmr_isVoiceFrame(ones.data()));

    // Also with a byte of it off
    ones[4] = 0x7F;
    REQUIRE_FALSE(dmr_isVoiceFrame(ones.data()));
}

TEST_CASE("DMR noise is rarely taken for voice", "[dmr][voice]")
{
    /*
     * A random 23-bit word is within one bit of a Golay codeword with
     * probability 24 * 2^12 / 2^23, about 1.2 %.
     */
    std::mt19937 rng(1234);
    const int count = 100000;
    int accepted = 0;

    for (int n = 0; n < count; n++) {
        uint8_t frame[DMR_VOICE_FRAME_LENGTH];
        for (auto &b : frame)
            b = rng() & 0xFF;

        if (dmr_isVoiceFrame(frame))
            accepted++;
    }

    REQUIRE(accepted > count * 0.008);
    REQUIRE(accepted < count * 0.016);
}
