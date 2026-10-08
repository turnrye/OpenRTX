/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/DMR/VoiceFrame.h"
#include <string.h>

const uint8_t DMR_VOICE_SILENCE_FRAME[DMR_VOICE_FRAME_LENGTH] = {
    0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B
};

/*
 * The DMR AMBE de-interleave. The 72 on-air bits of a frame are scattered into
 * the four codewords (a flat [4][24] buffer) by these fixed position tables.
 * This is the standard DMR AMBE frame layout.
 */
static const uint8_t ambeDeintA[36] = { 23, 34, 22, 33, 21, 32, 20, 31, 19,
                                        30, 18, 29, 17, 28, 16, 27, 15, 26,
                                        14, 25, 13, 24, 12, 58, 11, 57, 10,
                                        56, 9,  55, 8,  54, 7,  53, 6,  52 };
static const uint8_t ambeDeintB[36] = { 5,  51, 4,  50, 3,  49, 2,  48, 1,
                                        85, 0,  84, 46, 83, 45, 82, 44, 81,
                                        43, 80, 42, 79, 41, 78, 40, 77, 39,
                                        76, 38, 75, 37, 74, 36, 73, 35, 72 };

/*
 * Golay(23,12) syndrome of a 23-bit codeword, generator 0xC75. Zero for a valid
 * codeword.
 */
static uint32_t golaySyndrome(uint32_t cw)
{
    for (int i = 22; i >= 11; i--) {
        if (cw & (1u << i))
            cw ^= (uint32_t)0xC75 << (i - 11);
    }
    return cw & 0x7FF;
}

static int popcount11(uint32_t x)
{
    int n = 0;
    while (x) {
        n += x & 1;
        x >>= 1;
    }
    return n;
}

/**
 * Estimate the number of bit errors in a 23-bit Golay codeword (capped at 4).
 *
 * @param bits: 23 codeword bits, most significant first.
 */
static int golayErrors(const uint8_t *bits)
{
    uint32_t cw = 0;
    for (int i = 0; i < 23; i++)
        cw = (cw << 1) | bits[i];

    int s = popcount11(golaySyndrome(cw));
    if (s <= 3)
        return s;

    for (int i = 0; i < 23; i++) {
        int s2 = popcount11(golaySyndrome(cw ^ (1u << i)));
        if (s2 <= 2)
            return 1 + s2;
    }
    return 4;
}

uint8_t dmr_voiceFrameErrors(const uint8_t *frame)
{
    uint8_t cw[96];
    memset(cw, 0, sizeof(cw));

    int pos = 0;
    for (int i = 0; i < DMR_VOICE_FRAME_LENGTH; i++) {
        for (int j = 7; j > 0; j -= 2) {
            cw[ambeDeintA[pos]] = (frame[i] >> j) & 1;
            cw[ambeDeintB[pos]] = (frame[i] >> (j - 1)) & 1;
            pos++;
        }
    }

    uint8_t c0[23];
    for (int i = 0; i < 23; i++)
        c0[i] = cw[23 - i]; /* codeword-0 bits 23..1, MSB first */

    return golayErrors(c0);
}

bool dmr_isVoiceFrame(const uint8_t *frame)
{
    int ones = 0;
    for (int i = 0; i < DMR_VOICE_FRAME_LENGTH; i++) {
        if (frame[i] == 0xFF)
            ones++;
    }
    if (ones >= DMR_VOICE_FRAME_LENGTH - 1)
        return false;

    return dmr_voiceFrameErrors(frame) <= 1;
}
