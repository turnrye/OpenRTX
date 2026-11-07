/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APRS_SIGNAL_H
#define APRS_SIGNAL_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * Bell 202 AFSK1200 test signals, built here rather than with the
 * production modulator so the tests control what the production
 * demodulator is fed: tone levels, timing error and framing.
 */
struct AprsSignalParams {
    uint32_t rate = 9600;      ///< Sample rate the signal is made at.
    float markPeak = 300.0f;   ///< Peak of the 1200 Hz tone.
    float spacePeak = 300.0f;  ///< Peak of the 2200 Hz tone.
    float clockError = 0.0f;   ///< Sender's clock error, as a fraction.
    unsigned flagsBefore = 32; ///< Opening flags, 6.7 ms each.
    unsigned flagsAfter = 4;   ///< Closing flags.
};

/**
 * One frame on the air: flags, the stuffed and NRZI-coded frame with its
 * frame check sequence, flags. The frame is given without its FCS, as the
 * builder produces it.
 */
std::vector<float> aprsSignal(const uint8_t *frame, size_t len,
                              const AprsSignalParams &params);

/**
 * A distinct, well-formed APRS frame for test @p index: addresses and an
 * info field that vary in length and content, some with runs of ones that
 * need bit stuffing.
 */
std::vector<uint8_t> aprsTestFrame(size_t index);

#endif // APRS_SIGNAL_H
