/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <cmath>
#include <cstdio>
#include <cstring>

#include "APRSSignal.hpp"
#include "core/crc.h"
#include "protocols/APRS/packet.h"

static constexpr uint8_t FLAG = 0x7e;
static constexpr double BAUD = 1200.0;
static constexpr double MARK_HZ = 1200.0;
static constexpr double SPACE_HZ = 2200.0;

/** HDLC bits, least significant first, stuffed between the flags. */
static std::vector<uint8_t> hdlcBits(const uint8_t *frame, size_t len,
                                     unsigned flagsBefore, unsigned flagsAfter)
{
    std::vector<uint8_t> bits;
    unsigned ones = 0;

    auto emit = [&](uint8_t byte, bool stuff) {
        for (int i = 0; i < 8; i++) {
            uint8_t bit = (byte >> i) & 1;
            bits.push_back(bit);
            if (!stuff)
                continue;
            ones = bit ? ones + 1 : 0;
            if (ones == 5) {
                bits.push_back(0);
                ones = 0;
            }
        }
    };

    for (unsigned i = 0; i < flagsBefore; i++)
        emit(FLAG, false);

    ones = 0;
    for (size_t i = 0; i < len; i++)
        emit(frame[i], true);
    const uint16_t fcs = crc_hdlc(frame, len);
    emit((uint8_t)(fcs & 0xff), true);
    emit((uint8_t)(fcs >> 8), true);

    for (unsigned i = 0; i < flagsAfter; i++)
        emit(FLAG, false);

    return bits;
}

std::vector<float> aprsSignal(const uint8_t *frame, size_t len,
                              const AprsSignalParams &p)
{
    std::vector<uint8_t> bits = hdlcBits(frame, len, p.flagsBefore,
                                         p.flagsAfter);
    std::vector<float> out;

    const double speed = 1.0 + p.clockError;
    const double samplesPerBit = p.rate / (BAUD * speed);
    double phase = 0.0;
    double due = 0.0;
    size_t n = 0;
    int level = 1; /* NRZI: a zero flips the tone, a one holds it */

    for (uint8_t bit : bits) {
        if (bit == 0)
            level ^= 1;

        const double hz = (level ? MARK_HZ : SPACE_HZ) * speed;
        const double step = 2.0 * M_PI * hz / p.rate;
        const float peak = level ? p.markPeak : p.spacePeak;

        due += samplesPerBit;
        for (; n < (size_t)std::llround(due); n++) {
            out.push_back(peak * (float)std::sin(phase));
            phase += step;
            if (phase >= 2.0 * M_PI)
                phase -= 2.0 * M_PI;
        }
    }

    return out;
}

std::vector<uint8_t> aprsTestFrame(size_t index)
{
    static const char *const sources[] = { "N2BP-7",    "W1AW",  "KD2ABC-9",
                                           "VE3XYZ-15", "G0ABC", "JA1ZZZ-2" };
    static const char *const paths[] = { "WIDE1-1,WIDE2-1", "", "WIDE2-2",
                                         "N2BP*,WIDE1-1", "WIDE1-1" };
    /* Runs of ones in '~' (0x7e) and 0xff exercise bit stuffing. */
    static const char *const bodies[] = {
        ":N0CALL   :hello{01",
        "!4903.50N/07201.75W-OpenRTX test ~~~~ position",
        ">status with ones \xff\xff\xff and more text to make it long",
        ":W1AW     :ack01",
        "=4903.50N/07201.75W[PHG5132/A=000250 some comment text 77777",
        ";OBJECT   *111111z4903.50N/07201.75W-",
    };

    const char *src = sources[index % 6];
    const char *path = paths[(index / 6) % 5];
    const char *body = bodies[index % 6];

    /* Pad the info field so lengths vary, staying inside what the decoder
     * can hold: its APRS_PACLEN buffer includes the two FCS bytes. */
    char info[APRS_PACLEN];
    size_t len = strlen(body);
    memcpy(info, body, len);
    size_t pad = (index * 7) % 50;
    for (size_t i = 0; (i < pad) && (len < 90); i++)
        info[len++] = (char)('0' + ((index + i) % 10));

    std::vector<uint8_t> frame(APRS_PACLEN);
    ssize_t n = aprsFrame_build(frame.data(), frame.size(), "APORT1", src, path,
                                info, len);
    frame.resize((size_t)n);
    return frame;
}
