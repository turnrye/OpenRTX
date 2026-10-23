/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The DMR voice pipeline against fakes of the audio paths, the audio streams
 * and the vocoder: received frames decoded and played in order with silence
 * in between, the playback queue's depth, and microphone frames encoded, or
 * replaced by the silence frame when the gate finds no voice in them.
 */

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include "core/dmr_audio.h"
#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "core/dmr_debug.h"
#include "interfaces/dmr_codec.h"
#include "protocols/DMR/VoiceFrame.h"
}

/* ---------------------------------------------------------------- the fakes */

static std::mutex fakeMutex;
static std::condition_variable fakeCond;

static bool codecAvailable = true;
static int pathsRequested, pathsReleased;
static enum PathStatus pathStatus = PATH_OPEN; // What every path reports

static stream_sample_t outBuf[DMR_CODEC_PCM_SAMPLES];
static std::vector<int> played; // First sample of each buffer played
static bool outputHeld;         // Output stream stalled by the scenario

static int16_t micLevel;        // Amplitude of the microphone square wave
static stream_sample_t inBuf[DMR_CODEC_PCM_SAMPLES];
static uint8_t encodeCount;

extern "C" {

pathId audioPath_request(enum AudioSource source, enum AudioSink sink,
                         enum AudioPriority prio)
{
    (void)source;
    (void)sink;
    (void)prio;
    std::lock_guard<std::mutex> lk(fakeMutex);
    return ++pathsRequested;
}

enum PathStatus audioPath_getStatus(const pathId id)
{
    (void)id;
    std::lock_guard<std::mutex> lk(fakeMutex);
    return pathStatus;
}

void audioPath_release(const pathId id)
{
    (void)id;
    std::lock_guard<std::mutex> lk(fakeMutex);
    pathsReleased++;
}

streamId audioStream_start(const pathId path, stream_sample_t *const buf,
                           const size_t length, const uint32_t sampleRate,
                           const uint8_t mode)
{
    (void)path;
    (void)buf;
    (void)length;
    (void)sampleRate;
    return ((mode & STREAM_OUTPUT) != 0) ? 2 : 1;
}

void audioStream_stop(const streamId id)
{
    (void)id;
}

void audioStream_terminate(const streamId id)
{
    (void)id;
}

stream_sample_t *outputStream_getIdleBuffer(const streamId id)
{
    (void)id;
    return outBuf;
}

/* Play the idle buffer: note its first sample, one buffer per millisecond. */
bool outputStream_sync(const streamId id, const bool bufChanged)
{
    (void)id;
    {
        std::unique_lock<std::mutex> lk(fakeMutex);
        fakeCond.wait(lk, [] { return outputHeld == false; });
        if (bufChanged)
            played.push_back(outBuf[0]);
    }
    fakeCond.notify_all();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return true;
}

/* One frame of a square wave at the microphone level, every millisecond. */
dataBlock_t inputStream_getData(streamId id)
{
    (void)id;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::lock_guard<std::mutex> lk(fakeMutex);
    for (int i = 0; i < DMR_CODEC_PCM_SAMPLES; i++)
        inBuf[i] = ((i % 2) == 0) ? micLevel : -micLevel;
    dataBlock_t block = { inBuf, DMR_CODEC_PCM_SAMPLES };
    return block;
}

bool dmr_codec_available(void)
{
    return codecAvailable;
}

bool dmr_codec_init(void)
{
    return true;
}

/* Decoded audio carries the frame's first byte, plus one, in every sample. */
void dmr_codec_decode(int16_t pcm[DMR_CODEC_PCM_SAMPLES],
                      const uint8_t frame[DMR_CODEC_FRAME_BYTES])
{
    for (int i = 0; i < DMR_CODEC_PCM_SAMPLES; i++)
        pcm[i] = frame[0] + 1;
}

/* Encoded frames are marked 0xE0 and numbered. */
void dmr_codec_encode(uint8_t frame[DMR_CODEC_FRAME_BYTES],
                      const int16_t pcm[DMR_CODEC_PCM_SAMPLES])
{
    (void)pcm;
    memset(frame, 0, DMR_CODEC_FRAME_BYTES);
    frame[0] = 0xE0;
    frame[1] = encodeCount++;
}

void dmrDebug_log(const enum dmrDebugChannel channel, const char *fmt, ...)
{
    (void)channel;
    (void)fmt;
}

} // extern "C"

/* ---------------------------------------------------------------- scenarios */

static void resetFakes()
{
    std::lock_guard<std::mutex> lk(fakeMutex);
    codecAvailable = true;
    pathsRequested = pathsReleased = 0;
    pathStatus = PATH_OPEN;
    played.clear();
    outputHeld = false;
    micLevel = 0;
    encodeCount = 0;
}

/* The decoded frames played so far, by their first byte, silence left out. */
static std::vector<int> playedFrames()
{
    std::lock_guard<std::mutex> lk(fakeMutex);
    std::vector<int> out;
    for (int s : played)
        if (s != 0)
            out.push_back(s - 1);
    return out;
}

/* Wait for a condition, for up to two seconds. */
template <typename F> static bool waitFor(F condition)
{
    for (int i = 0; i < 2000; i++) {
        if (condition())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return condition();
}

static void setPathStatus(enum PathStatus status)
{
    std::lock_guard<std::mutex> lk(fakeMutex);
    pathStatus = status;
}

static int released()
{
    std::lock_guard<std::mutex> lk(fakeMutex);
    return pathsReleased;
}

static void setOutputHeld(bool held)
{
    {
        std::lock_guard<std::mutex> lk(fakeMutex);
        outputHeld = held;
    }
    fakeCond.notify_all();
}

static std::vector<uint8_t> frame(uint8_t first)
{
    std::vector<uint8_t> f(DMR_CODEC_FRAME_BYTES, 0);
    f[0] = first;
    return f;
}

TEST_CASE("DMR playback does not start without the vocoder", "[dmr][audio]")
{
    resetFakes();
    codecAvailable = false;
    REQUIRE_FALSE(dmrAudio_startRx());
    REQUIRE_FALSE(dmrAudio_running());
    REQUIRE(pathsRequested == 0);
}

TEST_CASE("DMR playback plays received frames in order, silence in between",
          "[dmr][audio]")
{
    resetFakes();
    REQUIRE(dmrAudio_startRx());
    REQUIRE(dmrAudio_running());

    for (uint8_t n = 1; n <= 3; n++)
        dmrAudio_pushFrame(frame(n).data());

    REQUIRE(waitFor([] { return playedFrames().size() == 3; }));
    REQUIRE(playedFrames() == std::vector<int>({ 1, 2, 3 }));

    // Nothing queued: the slots go out silent
    REQUIRE(waitFor([] {
        std::lock_guard<std::mutex> lk(fakeMutex);
        return (played.size() > 6) && (played.back() == 0);
    }));

    dmrAudio_stopRx();
    REQUIRE_FALSE(dmrAudio_running());
    REQUIRE(pathsReleased == 1);

    dmrAudio_stopRx(); // A second stop does nothing
    REQUIRE(pathsReleased == 1);
}

TEST_CASE("DMR playback queue holds eight frames and drops the rest",
          "[dmr][audio]")
{
    resetFakes();
    setOutputHeld(true);
    REQUIRE(dmrAudio_startRx());

    // The playback thread is stuck on the stalled output with nothing taken
    for (uint8_t n = 1; n <= 10; n++)
        dmrAudio_pushFrame(frame(n).data());

    setOutputHeld(false);
    REQUIRE(waitFor([] { return playedFrames().size() >= 8; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(playedFrames() == std::vector<int>({ 1, 2, 3, 4, 5, 6, 7, 8 }));

    dmrAudio_stopRx();
}

TEST_CASE("DMR playback ignores frames while it is not running", "[dmr][audio]")
{
    resetFakes();
    dmrAudio_pushFrame(frame(5).data());
    REQUIRE(dmrAudio_startRx());
    dmrAudio_pushFrame(frame(6).data());
    REQUIRE(waitFor([] { return playedFrames().size() == 1; }));
    REQUIRE(playedFrames() == std::vector<int>({ 6 }));
    dmrAudio_stopRx();
}

TEST_CASE("DMR playback gives the speaker back when another path takes it",
          "[dmr][audio]")
{
    resetFakes();
    REQUIRE(dmrAudio_startRx());

    // A voice prompt takes the speaker over: playback ends on its own
    setPathStatus(PATH_SUSPENDED);
    REQUIRE(waitFor([] { return dmrAudio_running() == false; }));

    // Asked again while the speaker is still taken: the old path is released
    // and the new one, not open, at once
    REQUIRE_FALSE(dmrAudio_startRx());
    REQUIRE(released() == 2);

    // Once the speaker is free, playback runs again
    setPathStatus(PATH_OPEN);
    REQUIRE(dmrAudio_startRx());
    REQUIRE(dmrAudio_running());
    dmrAudio_pushFrame(frame(4).data());
    REQUIRE(waitFor([] { return playedFrames().size() == 1; }));

    // Ending on its own again, then stopping, releases that path too
    setPathStatus(PATH_SUSPENDED);
    REQUIRE(waitFor([] { return dmrAudio_running() == false; }));
    dmrAudio_stopRx();
    REQUIRE(released() == 3);
    REQUIRE(pathsRequested == 3);
    dmrAudio_stopRx();
    REQUIRE(released() == 3);
}

TEST_CASE("DMR capture does not start without the vocoder", "[dmr][audio]")
{
    resetFakes();
    codecAvailable = false;
    REQUIRE_FALSE(dmrAudio_startTx());
    REQUIRE_FALSE(dmrAudio_txRunning());
    uint8_t f[DMR_CODEC_FRAME_BYTES];
    REQUIRE_FALSE(dmrAudio_popFrame(f));
}

TEST_CASE("DMR capture encodes voice and sends silence without it",
          "[dmr][audio]")
{
    resetFakes();
    {
        std::lock_guard<std::mutex> lk(fakeMutex);
        micLevel = 8000; // Well above the gate
    }
    REQUIRE(dmrAudio_startTx());

    // Voice: encoded frames, in order
    std::vector<uint8_t> numbers;
    REQUIRE(waitFor([&numbers] {
        uint8_t f[DMR_CODEC_FRAME_BYTES];
        while (dmrAudio_popFrame(f)) {
            REQUIRE(f[0] == 0xE0);
            numbers.push_back(f[1]);
        }
        return numbers.size() >= 4;
    }));
    // In order; frames can be skipped while the queue is full
    for (size_t i = 1; i < numbers.size(); i++) {
        uint8_t step = numbers[i] - numbers[i - 1];
        REQUIRE(((step >= 1) && (step < 128)));
    }

    // Quiet: once the gate has closed, the silence frame goes out
    {
        std::lock_guard<std::mutex> lk(fakeMutex);
        micLevel = 0;
    }
    REQUIRE(waitFor([] {
        uint8_t f[DMR_CODEC_FRAME_BYTES];
        while (dmrAudio_popFrame(f)) {
            if (memcmp(f, DMR_VOICE_SILENCE_FRAME, DMR_CODEC_FRAME_BYTES) == 0)
                return true;
        }
        return false;
    }));

    dmrAudio_stopTx();
    REQUIRE_FALSE(dmrAudio_txRunning());
    REQUIRE(pathsReleased == 1);
    dmrAudio_stopTx();
    REQUIRE(pathsReleased == 1);
}

TEST_CASE("DMR capture gives the microphone back when another path takes it",
          "[dmr][audio]")
{
    resetFakes();
    {
        std::lock_guard<std::mutex> lk(fakeMutex);
        micLevel = 8000;
    }
    REQUIRE(dmrAudio_startTx());
    REQUIRE(waitFor([] {
        std::lock_guard<std::mutex> lk(fakeMutex);
        return encodeCount > 2;
    }));

    // The path is taken over: capture ends on its own, keeping the path
    setPathStatus(PATH_SUSPENDED);
    REQUIRE(waitFor([] { return dmrAudio_txRunning() == false; }));
    REQUIRE(released() == 0);

    // Stopping releases it and drops what was left encoded
    dmrAudio_stopTx();
    REQUIRE(released() == 1);
    uint8_t f[DMR_CODEC_FRAME_BYTES];
    REQUIRE_FALSE(dmrAudio_popFrame(f));
    dmrAudio_stopTx();
    REQUIRE(released() == 1);
}
