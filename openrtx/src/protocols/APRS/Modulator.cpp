/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/APRS/Modulator.hpp"

#include <cerrno>
#include <cstring>

#if defined(PLATFORM_LINUX)
#include <cstdio>
#endif

using namespace APRS;

constexpr std::array<int16_t, Modulator::QUARTER_STEPS + 1>
    Modulator::SINE_QUARTER;

Modulator::Modulator() :
    idleBuffer(nullptr), outStream(0), outPath(0), phase(0), step(0),
    symbolLeft(0), txRunning(false)
{
}

Modulator::~Modulator()
{
    terminate();
}

void Modulator::init()
{
    basebandBuffer = std::make_unique<int16_t[]>(2 * BLOCK_SAMPLES);
    idleBuffer = basebandBuffer.get();
    txRunning = false;
}

void Modulator::terminate()
{
    stop();
    basebandBuffer.reset();
    idleBuffer = nullptr;
}

int Modulator::start()
{
    if (txRunning)
        return 0;

    // Each transmission starts from the same tone state
    phase = 0;
    symbolLeft = 0;
    idleBuffer = basebandBuffer.get();

#ifndef PLATFORM_LINUX
    outPath = audioPath_request(SOURCE_MCU, SINK_RTX, PRIO_TX);
    if (outPath < 0)
        return -EIO;

    outStream = audioStream_start(outPath, basebandBuffer.get(),
                                  2 * BLOCK_SAMPLES, TX_SAMPLE_RATE,
                                  STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    if (outStream < 0) {
        audioPath_release(outPath);
        return -EIO;
    }

    idleBuffer = outputStream_getIdleBuffer(outStream);
#endif

    txRunning = true;
    return 0;
}

int16_t Modulator::sine(uint8_t phase)
{
    const uint8_t quadrant = phase / QUARTER_STEPS;
    const uint8_t index = phase % QUARTER_STEPS;

    switch (quadrant) {
        case 0:
            return SINE_QUARTER[index];
        case 1:
            return SINE_QUARTER[QUARTER_STEPS - index];
        case 2:
            return (int16_t)(-SINE_QUARTER[index]);
        default:
            return (int16_t)(-SINE_QUARTER[QUARTER_STEPS - index]);
    }
}

size_t Modulator::generate(FrameEncoder &encoder, stream_sample_t *out,
                           size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (symbolLeft == 0) {
            uint8_t tone;
            if (!encoder.nextSymbol(tone))
                return i;

            step = (tone != 0) ? phaseStep(MARK_HZ) : phaseStep(SPACE_HZ);
            symbolLeft = SAMPLES_PER_SYMBOL;
        }

        // The top byte of the accumulator indexes the table; phase is never
        // reset between symbols, which keeps the FSK continuous
        const int32_t s = sine((uint8_t)(phase >> 24));
        out[i] = (int16_t)((s * TX_AMPLITUDE) / SINE_PEAK);
        phase += step;
        symbolLeft--;
    }

    return len;
}

bool Modulator::sendBlock(FrameEncoder &encoder)
{
    const size_t n = generate(encoder, idleBuffer, BLOCK_SAMPLES);
    if (n == 0)
        return false;

    memset(&idleBuffer[n], 0, (BLOCK_SAMPLES - n) * sizeof(idleBuffer[0]));
    sendBaseband();

    return true;
}

void Modulator::stop()
{
    if (!txRunning)
        return;

#ifndef PLATFORM_LINUX
    audioStream_stop(outStream);
    audioPath_release(outPath);
#endif

    txRunning = false;
    idleBuffer = basebandBuffer.get();
}

#ifndef PLATFORM_LINUX
void Modulator::sendBaseband()
{
    if (audioPath_getStatus(outPath) != PATH_OPEN)
        return;

    // Wait for the other half to finish playing before claiming it
    outputStream_sync(outStream, true);
    idleBuffer = outputStream_getIdleBuffer(outStream);
}
#else
void Modulator::sendBaseband()
{
    // The emulator has no transmit audio device, so the baseband goes to a
    // file, as M17::Modulator does
    FILE *out = fopen("/tmp/aprs_output.raw", "ab");
    if (out == nullptr)
        return;

    fwrite(idleBuffer, sizeof(idleBuffer[0]), BLOCK_SAMPLES, out);
    fclose(out);
}
#endif
