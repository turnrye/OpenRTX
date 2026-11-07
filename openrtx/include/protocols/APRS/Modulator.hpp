/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APRS_MODULATOR_H
#define APRS_MODULATOR_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "protocols/APRS/FrameEncoder.hpp"

/* Peak level of the generated tones, which sets the FM deviation: a target
 * may tune it in its hwconfig.h. The default was measured on an MD-UV3x0. */
#ifndef CONFIG_APRS_TX_AMPLITUDE
#define CONFIG_APRS_TX_AMPLITUDE 12000
#endif

namespace APRS
{

/**
 * Bell 202 AFSK1200 modulator: turns the symbols a FrameEncoder produces into
 * continuous-phase 1200 Hz and 2200 Hz tones, in integer arithmetic, and
 * streams them to the radio one block at a time, as M17::Modulator does with
 * its frames.
 */
class Modulator
{
public:
    /**
     * Constructor.
     */
    Modulator();

    /**
     * Destructor.
     */
    ~Modulator();

    /**
     * Allocate the baseband buffer.
     */
    void init();

    /**
     * Stop the modulator and release its buffer.
     */
    void terminate();

    /**
     * Open the output path and start the baseband stream.
     *
     * @return 0 on success, -EIO if the path or the stream cannot be opened.
     */
    int start();

    /**
     * Generate the tone for the encoder's next symbols. Phase carries on from
     * the previous call.
     *
     * @param encoder: source of the symbols.
     * @param out: destination for the samples.
     * @param len: room in out, in samples.
     * @return number of samples written; fewer than len once the encoder has
     *         run out of symbols.
     */
    size_t generate(FrameEncoder &encoder, stream_sample_t *out, size_t len);

    /**
     * Generate one block of baseband and hand it to the output stream,
     * waiting for the block before it to finish playing.
     *
     * @param encoder: source of the symbols.
     * @return true if a block was sent, false if the encoder had nothing left.
     */
    bool sendBlock(FrameEncoder &encoder);

    /**
     * Stop the baseband stream and release the output path.
     */
    void stop();

    static constexpr size_t TX_SAMPLE_RATE = 48000; ///< Baseband rate, as M17.
    static constexpr int32_t TX_AMPLITUDE = CONFIG_APRS_TX_AMPLITUDE; ///< Peak.

private:
    /**
     * Sine of a phase given as the top eight bits of the accumulator, scaled
     * to SINE_PEAK.
     */
    static int16_t sine(uint8_t phase);

    /**
     * Hand the filled idle buffer to the output stream.
     */
    void sendBaseband();

    static constexpr size_t SAMPLES_PER_SYMBOL = TX_SAMPLE_RATE
                                               / APRS_SYMBOL_RATE;
    static constexpr size_t BLOCK_SYMBOLS = 8;  ///< 6.7 ms of audio per block.
    static constexpr size_t BLOCK_SAMPLES = BLOCK_SYMBOLS * SAMPLES_PER_SYMBOL;
    static constexpr uint32_t MARK_HZ = 1200;   ///< Tone for a 1 level.
    static constexpr uint32_t SPACE_HZ = 2200;  ///< Tone for a 0 level.
    static constexpr int32_t SINE_PEAK = 32767;
    static constexpr size_t QUARTER_STEPS = 64; ///< Table steps per quadrant.

    /**
     * Phase increment per sample for a tone, as a 32-bit fraction of a turn.
     */
    static constexpr uint32_t phaseStep(uint32_t freq)
    {
        return (uint32_t)(((uint64_t)freq << 32) / TX_SAMPLE_RATE);
    }

    /**
     * First quadrant of a sine, sin(i * 90 / 64 degrees) * SINE_PEAK, with
     * the peak at the end so the other quadrants are its reflections.
     */
    static constexpr std::array<int16_t, QUARTER_STEPS + 1> SINE_QUARTER = {
        0,     804,   1608,  2410,  3212,  4011,  4808,  5602,  6393,  7179,
        7962,  8739,  9512,  10278, 11039, 11793, 12539, 13279, 14010, 14732,
        15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403,
        22005, 22594, 23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
        27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956, 30273, 30571,
        30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521,
        32609, 32678, 32728, 32757, 32767,
    };

    std::unique_ptr<int16_t[]> basebandBuffer; ///< Double baseband buffer.
    stream_sample_t *idleBuffer;               ///< Half free for generation.
    streamId outStream;                        ///< Baseband output stream.
    pathId outPath;                            ///< Baseband output path.
    uint32_t phase;                            ///< Tone phase accumulator.
    uint32_t step;                             ///< Phase step of the tone.
    size_t symbolLeft;                         ///< Samples left in symbol.
    bool txRunning;                            ///< Stream is open.
};

} /* APRS */

#endif /* APRS_MODULATOR_H */
