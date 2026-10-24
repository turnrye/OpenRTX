/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The tone player against fakes of the beeper, the clock and the speaker path.
 * The clock is simulated: sleeping moves it on at once, unless a scenario holds
 * it, in which case it moves only when the scenario says so, and a sleep then
 * just lets the player look again.
 */

#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#include "core/tones.h"
#include "core/audio_path.h"
#include "core/voicePrompts.h"
#include "interfaces/delays.h"
#include "interfaces/platform.h"

/* ---------------------------------------------------------------- the fakes */

static std::mutex fakeMutex;
static std::vector<std::pair<long long, uint16_t>> beeper; // Tick, Hz or 0
static std::atomic<long long> clockMs;
static std::atomic<bool> clockHeld;
static std::atomic<int> pathsOpen;
static std::atomic<bool> pathTaken; // Another source took the speaker over
static std::atomic<bool> beepBusy;

extern "C" {

void platform_beepStart(uint16_t freq)
{
    std::lock_guard<std::mutex> lk(fakeMutex);
    beeper.push_back({ clockMs.load(), freq });
}

void platform_beepStop()
{
    std::lock_guard<std::mutex> lk(fakeMutex);
    beeper.push_back({ clockMs.load(), 0 });
}

long long getTick()
{
    return clockMs.load();
}

void sleepFor(unsigned int seconds, unsigned int mseconds)
{
    if (clockHeld.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    else
        clockMs += (seconds * 1000) + mseconds;
}

pathId audioPath_request(enum AudioSource source, enum AudioSink sink,
                         enum AudioPriority prio)
{
    (void)source;
    (void)sink;
    (void)prio;
    pathsOpen += 1;
    return 1;
}

enum PathStatus audioPath_getStatus(const pathId id)
{
    (void)id;
    if (pathTaken.load())
        return PATH_SUSPENDED;
    return (pathsOpen.load() > 0) ? PATH_OPEN : PATH_CLOSED;
}

void audioPath_release(const pathId id)
{
    (void)id;
    pathsOpen -= 1;
}

bool vp_beepBusy()
{
    return beepBusy.load();
}
}

static void resetFakes()
{
    tones_stop();
    std::lock_guard<std::mutex> lk(fakeMutex);
    beeper.clear();
    clockMs = 0;
    clockHeld = false;
    pathsOpen = 0;
    pathTaken = false;
    beepBusy = false;
}

/* Wait, in real time, for the tone to end. */
static void waitSilent()
{
    for (int n = 0; tones_playing() && (n < 2000); n++)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    tones_stop();
}

static std::vector<std::pair<long long, uint16_t>> played()
{
    std::lock_guard<std::mutex> lk(fakeMutex);
    return beeper;
}

/* -------------------------------------------------------------- the tests */

TEST_CASE("Tones play an RTTTL tone note by note, on time", "[tones]")
{
    resetFakes();

    // The Motorola talk permit tone: G6, C6, G6 of 35 ms, then E6 of 52 ms
    REQUIRE(tones_playRtttl("permit:d=32,o=6,b=214:g,c,g,e."));
    waitSilent();

    const std::vector<std::pair<long long, uint16_t>> expected = {
        { 0, 1568 },  { 35, 0 },  { 35, 1047 },  { 70, 0 },
        { 70, 1568 }, { 105, 0 }, { 105, 1319 }, { 157, 0 }
    };
    REQUIRE(played() == expected);
    REQUIRE(pathsOpen == 0);
}

TEST_CASE("Tones keep quiet through a pause", "[tones]")
{
    resetFakes();

    const struct rtttl_note tone[] = { { 523, 100 }, { 0, 50 }, { 523, 100 } };
    REQUIRE(tones_playNotes(tone, 3));
    waitSilent();

    const std::vector<std::pair<long long, uint16_t>> expected = {
        { 0, 523 }, { 100, 0 }, { 150, 523 }, { 250, 0 }
    };
    REQUIRE(played() == expected);
}

TEST_CASE("Tones do not start over another tone or a beep", "[tones]")
{
    resetFakes();
    clockHeld = true;

    REQUIRE(tones_playRtttl("a:d=4,o=5,b=120:c"));
    REQUIRE(tones_playing());
    REQUIRE_FALSE(tones_playRtttl("b:d=4,o=5,b=120:e"));
    tones_stop();

    beepBusy = true;
    REQUIRE_FALSE(tones_playRtttl("b:d=4,o=5,b=120:e"));
    REQUIRE_FALSE(tones_playing());
    REQUIRE(pathsOpen == 0);
}

TEST_CASE("Tones stop when asked, silent at once", "[tones]")
{
    resetFakes();
    clockHeld = true;

    REQUIRE(tones_playRtttl("long:d=1,o=5,b=60:d#"));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(tones_playing());

    tones_stop();
    REQUIRE_FALSE(tones_playing());
    REQUIRE(played().back().second == 0);
    REQUIRE(pathsOpen == 0);

    // And the next one starts
    REQUIRE(tones_playRtttl("a:d=4,o=5,b=120:c"));
    tones_stop();
}

TEST_CASE("Tones end when the speaker is taken over", "[tones]")
{
    resetFakes();
    clockHeld = true;

    REQUIRE(tones_playRtttl("long:d=1,o=5,b=60:d#,d#"));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    pathTaken = true;
    for (int n = 0; tones_playing() && (n < 1000); n++)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE_FALSE(tones_playing());
    REQUIRE(played().size() == 2); // The first note only, stopped
    tones_stop();
}

TEST_CASE("Tones refuse an empty or oversized tone", "[tones]")
{
    resetFakes();

    const struct rtttl_note tone[1] = { { 523, 100 } };
    REQUIRE_FALSE(tones_playNotes(tone, 0));
    REQUIRE_FALSE(tones_playNotes(tone, RTTTL_MAX_NOTES + 1));
    REQUIRE_FALSE(tones_playRtttl("nothing:d=4,o=5,b=120:"));
    REQUIRE(pathsOpen == 0);
}
