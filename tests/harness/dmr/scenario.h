/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_HARNESS_SCENARIO_H
#define DMR_HARNESS_SCENARIO_H

/*
 * What the DMR driver scenarios of both radios share: register and burst
 * names, link control and payloads to hand the driver, and checks on what it
 * wrote to the chip.
 */

#include <cstdint>
#include <utility>
#include <vector>
#include "harness.h"

using namespace harness;
using Bytes = std::vector<uint8_t>;

/* A configuration register write: register, value. */
using RegWrite = std::pair<uint16_t, uint8_t>;

/* Register pages on the bus. */
static const uint8_t AUX = 1, DATA = 2, CONFIG = 4;

/* Burst status: sync class in bits 1:0, data type or frame number above. */
static const uint8_t HEADER = 0x12; // Voice LC header
static const uint8_t TERMINATOR = 0x22;
static const uint8_t FRAME_A = 0x11;
static const uint8_t FRAME_B = 0x20;
static const uint8_t EMPTY = 0x00; // Class 0 with nothing decoded
static const uint8_t NOISE = 0x03; // Class 3: nothing recognisable

/* Interrupt flags of register 0x82. */
static const uint8_t FLAG_BURST = 0x08, FLAG_LC = 0x10, FLAG_TICK = 0x20;

static const uint8_t CC = 1;

/* The link control of a group call, as the chip hands it over. */
static inline Bytes groupLc(uint32_t dst, uint32_t src)
{
    return { 0x00,
             0x00,
             0x00,
             (uint8_t)(dst >> 16),
             (uint8_t)(dst >> 8),
             (uint8_t)dst,
             (uint8_t)(src >> 16),
             (uint8_t)(src >> 8),
             (uint8_t)src,
             0x00,
             0x00,
             0x00 };
}

/* A voice frame received over the air, three times over: a voice payload. */
static inline Bytes voicePayload()
{
    Bytes f = { 0xB9, 0xCE, 0xA3, 0x76, 0x8F, 0xBB, 0x69, 0xCA, 0xCD };
    Bytes p;
    for (int i = 0; i < 3; i++)
        p.insert(p.end(), f.begin(), f.end());
    return p;
}

/* The last value written to a register, or -1 if it was not written. */
static inline int lastWrite(uint8_t page, uint16_t addr)
{
    std::vector<Write> ws = writes(page, addr);
    return ws.empty() ? -1 : ws.back().value;
}

/* The slot action written at each slot boundary, in order. */
static inline std::vector<uint8_t> slotActions()
{
    std::vector<uint8_t> v;
    for (const Write &w : writes(CONFIG, 0x41))
        v.push_back(w.value);
    return v;
}

/* The configuration register writes, in order, to any of the given registers. */
static inline std::vector<RegWrite>
configWrites(const std::vector<Write> &ws, const std::vector<uint16_t> &regs)
{
    std::vector<RegWrite> out;
    for (const Write &w : ws) {
        if (w.page != CONFIG)
            continue;
        for (uint16_t r : regs) {
            if (w.addr == r) {
                out.push_back({ w.addr, w.value });
                break;
            }
        }
    }
    return out;
}

/* Whether a sequence of writes appears, back to back, among others. */
static inline bool containsSequence(const std::vector<RegWrite> &ws,
                                    const std::vector<RegWrite> &seq)
{
    if (seq.size() > ws.size())
        return false;
    for (size_t i = 0; i + seq.size() <= ws.size(); i++) {
        bool match = true;
        for (size_t k = 0; k < seq.size() && match; k++)
            match = (ws[i + k] == seq[k]);
        if (match)
            return true;
    }
    return false;
}

/* Whether any configuration register in the list was written. */
static inline bool wroteAny(const std::vector<Write> &ws,
                            const std::vector<uint16_t> &regs)
{
    return configWrites(ws, regs).empty() == false;
}

/* Start the driver for a scenario and stop it, tracing what it did, after. */
struct Driver {
    Driver(uint8_t cc = CC)
    {
        reset();
        start(cc);
    }
    ~Driver()
    {
        stop();
        trace();
    }
};

#endif
