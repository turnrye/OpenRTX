/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <new>
#include <cstddef>
#include <cstring>
#include "protocols/M17/Modulator.hpp"
#include "protocols/M17/Utils.hpp"
#include "protocols/M17/DSP.hpp"

#if defined(PLATFORM_LINUX)
#include <stdio.h>
#endif

#include "hwconfig.h"

#ifdef CONFIG_M17_RAW_TX
/*
 * The radio takes M17 frame bytes, as dibits, instead of baseband: its
 * modulator shapes and sends them itself.
 */
extern "C" void radio_rawTxPush(const uint8_t *data, const size_t len);
extern "C" void radio_rawTxDrain();
#endif

using namespace M17;


Modulator::Modulator()
{

}

Modulator::~Modulator()
{
    terminate();
}

void Modulator::init()
{
    /*
     * Allocate a chunk of memory to contain two complete buffers for baseband
     * audio.
     */

    baseband_buffer = std::make_unique< int16_t[] >(2 * FRAME_SAMPLES);
    idleBuffer      = baseband_buffer.get();
    txRunning       = false;
    #if defined(PLATFORM_MD3x0) || defined(PLATFORM_MDUV3x0)
    pwmComp.reset();
    #endif
}

void Modulator::terminate()
{
    // Terminate an ongoing stream, if present
    if(txRunning)
    {
        audioStream_terminate(outStream);
        txRunning = false;
    }

    // Always ensure that outgoing audio path is closed
    audioPath_release(outPath);

    // Deallocate memory.
    baseband_buffer.reset();
}

bool Modulator::start()
{
    if(txRunning)
        return true;

    #ifdef CONFIG_M17_RAW_TX
    txRunning = true;
    return true;
    #endif

    #ifndef PLATFORM_LINUX
    outPath = audioPath_request(SOURCE_MCU, SINK_RTX, PRIO_TX);
    if(outPath < 0)
        return false;

    outStream = audioStream_start(outPath, baseband_buffer.get(),
                                  2*FRAME_SAMPLES, TX_SAMPLE_RATE,
                                  STREAM_OUTPUT | BUF_CIRC_DOUBLE);

    if(outStream < 0)
        return false;

    idleBuffer = outputStream_getIdleBuffer(outStream);
    #endif

    txRunning = true;

    return true;
}

void Modulator::sendPreamble()
{
    #ifdef CONFIG_M17_RAW_TX
    // 160 ms of +3 -3 (four frames): dibits 01 11 01 11
    uint8_t pre[48];
    memset(pre, 0x77, sizeof(pre));
    for(size_t frame = 0; frame < 4; frame++)
        radio_rawTxPush(pre, sizeof(pre));
    return;
    #endif

    // Fill symbol buffer with preamble, made of alternated +3 and -3 symbols
    for(size_t i = 0; i < symbols.size(); i += 2)
    {
        symbols[i]     = +3;
        symbols[i + 1] = -3;
    }

    // Generate and transmit a 160 ms preamble (four 40 ms frames).
    for (size_t frame = 0; frame < 4; frame++)
    {
        symbolsToBaseband();
        sendBaseband();
    }
}

void Modulator::sendFrame(const frame_t& frame)
{
    #ifdef CONFIG_M17_RAW_TX
    // Inverted phase: each dibit's sign (high) bit flipped
    frame_t out = frame;
    if(invPhase)
    {
        for(auto& b : out)
            b ^= 0xAA;
    }
    radio_rawTxPush(out.data(), out.size());
    return;
    #endif

    auto it = symbols.begin();
    for(size_t i = 0; i < frame.size(); i++)
    {
        auto sym = byteToSymbols(frame[i]);
        it       = std::copy(sym.begin(), sym.end(), it);
    }

    symbolsToBaseband();
    sendBaseband();
}

void Modulator::stop()
{
    if(txRunning == false)
        return;

    #ifdef CONFIG_M17_RAW_TX
    radio_rawTxDrain();
    txRunning = false;
    return;
    #endif

    audioStream_stop(outStream);
    txRunning  = false;
    idleBuffer = baseband_buffer.get();
    audioPath_release(outPath);

    #if defined(PLATFORM_MD3x0) || defined(PLATFORM_MDUV3x0)
    pwmComp.reset();
    #endif
}

void Modulator::invertPhase(const bool status)
{
    invPhase = status;
}


void Modulator::symbolsToBaseband()
{
    memset(idleBuffer, 0x00, FRAME_SAMPLES * sizeof(stream_sample_t));

    for(size_t i = 0; i < symbols.size(); i++)
    {
        idleBuffer[i * 10] = symbols[i];
    }

    for(size_t i = 0; i < FRAME_SAMPLES; i++)
    {
        float elem    = static_cast< float >(idleBuffer[i]);
        elem          = M17::rrc_48k(elem * RRC_GAIN) - RRC_OFFSET;
        #if defined(PLATFORM_MD3x0) || defined(PLATFORM_MDUV3x0)
        elem          = pwmComp(elem);
        #endif
        if(invPhase) elem = 0.0f - elem;    // Invert signal phase
        idleBuffer[i] = static_cast< int16_t >(elem);
    }
}

#ifndef PLATFORM_LINUX
void Modulator::sendBaseband()
{
    if(txRunning == false) return;
    if(audioPath_getStatus(outPath) != PATH_OPEN) return;

    // Transmission is ongoing, syncronise with stream end before proceeding
    outputStream_sync(outStream, true);
    idleBuffer = outputStream_getIdleBuffer(outStream);
}
#else
void Modulator::sendBaseband()
{
    FILE *outfile = fopen("/tmp/m17_output.raw", "ab");

    for(size_t i = 0; i < FRAME_SAMPLES; i++)
    {
        auto s = idleBuffer[i];
        fwrite(&s, sizeof(s), 1, outfile);
    }

    fclose(outfile);
}
#endif
