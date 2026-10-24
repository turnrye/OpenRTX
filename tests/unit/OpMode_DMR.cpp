/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The DMR operating mode against fakes of the radio, the platform, the DMR
 * baseband and the DMR audio, which record what it asks of them: how it
 * configures the baseband from the rtx status, how it hands over between
 * reception and transmission, and when it plays a call and runs the
 * microphone.
 */

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "interfaces/dmr.h"
#include "core/dmr_audio.h"
#include "core/dmr_debug.h"
#include "rtx/OpMode_DMR.hpp"

/* ---------------------------------------------------------------- the fakes */

static std::vector<std::string> calls; // Radio, baseband and audio calls
static bool ptt;                       // PTT pressed
static bool baseband_tx;               // dmr_txActive
static enum dmrTxReadiness readiness;  // dmr_txReadiness
static bool wakeupHeld;                // The wake-up burst stays in progress
static long long clockMs;              // getTick, moved on by sleepFor
static struct dmrRxState basebandRx;   // What dmr_task hands out
static bool playing, capturing;        // Playback and microphone running
static bool speakerBusy;               // Playback cannot start
static int playbackAsks;               // dmrAudio_startRx calls
static int playbackStops;              // dmrAudio_stopRx calls
static struct dmrConfig lastConfig;    // Last init or configure
static int inits, configures;
static bool startGroup;
static uint32_t startDst;

extern "C" {

bool platform_getPttStatus()
{
    return ptt;
}

void platform_ledOn(led_t led)
{
    (void)led;
}

void platform_ledOff(led_t led)
{
    (void)led;
}

void radio_enableRx()
{
    calls.push_back("enableRx");
}

void radio_enableTx()
{
    calls.push_back("enableTx");
}

void radio_disableRtx()
{
    calls.push_back("disableRtx");
}

void sleepFor(unsigned int seconds, unsigned int mseconds)
{
    clockMs += (seconds * 1000) + mseconds;
}

long long getTick()
{
    return clockMs;
}

static int refusals; // "tx: refused" lines logged

void dmrDebug_log(const enum dmrDebugChannel channel, const char *fmt, ...)
{
    (void)channel;
    if (strstr(fmt, "tx: refused") != NULL)
        refusals++;
}

void dmr_init(const struct dmrConfig *config)
{
    lastConfig = *config;
    inits++;
}

void dmr_configure(const struct dmrConfig *config)
{
    lastConfig = *config;
    configures++;
}

void dmr_terminate()
{
    calls.push_back("terminate");
}

void dmr_task(struct dmrRxState *rx)
{
    *rx = basebandRx;
}

void dmr_startTx(const bool groupCall, const uint32_t dstId)
{
    calls.push_back("startTx");
    startGroup = groupCall;
    startDst = dstId;
    baseband_tx = true;
}

void dmr_stopTx()
{
}

void dmr_txTask()
{
}

enum dmrTxReadiness dmr_txReadiness()
{
    return readiness;
}

void dmr_startWakeup()
{
    calls.push_back("wakeup");
    baseband_tx = wakeupHeld;
}

bool dmr_txActive()
{
    return baseband_tx;
}

bool dmrAudio_startRx(void)
{
    playbackAsks++;
    if (speakerBusy || playing)
        return playing;
    calls.push_back("playOn");
    playing = true;
    return true;
}

void dmrAudio_stopRx(void)
{
    playbackStops++;
    if (playing)
        calls.push_back("playOff");
    playing = false;
}

bool dmrAudio_running(void)
{
    return playing;
}

bool dmrAudio_startTx(void)
{
    if (capturing == false)
        calls.push_back("micOn");
    capturing = true;
    return true;
}

void dmrAudio_stopTx(void)
{
    if (capturing)
        calls.push_back("micOff");
    capturing = false;
}

} // extern "C"

/* ---------------------------------------------------------------- scenarios */

static void resetFakes()
{
    calls.clear();
    ptt = false;
    baseband_tx = false;
    readiness = DMR_TX_READY;
    wakeupHeld = false;
    clockMs = 0;
    memset(&basebandRx, 0, sizeof(basebandRx));
    playing = capturing = speakerBusy = false;
    playbackAsks = playbackStops = 0;
    refusals = 0;
    memset(&lastConfig, 0, sizeof(lastConfig));
    inits = configures = 0;
    startGroup = false;
    startDst = 0;
}

static rtxStatus_t channel(freq_t rx, freq_t tx)
{
    rtxStatus_t s;
    memset(&s, 0, sizeof(s));
    s.opMode = OPMODE_DMR;
    s.opStatus = OFF;
    s.rxFrequency = rx;
    s.txFrequency = tx;
    s.dmrId = 3101003;
    s.dmrTalkgroup = 9;
    s.dmrColorCode = 1;
    s.dmrTimeslot = 0;
    s.dmrRxPromisc = 1;
    return s;
}

/* Run the mode until it settles in a state, at most a few steps. */
static void run(OpMode_DMR &mode, rtxStatus_t &s, int steps = 4)
{
    for (int i = 0; i < steps; i++)
        mode.update(&s, false);
}

TEST_CASE("DMR opmode configures the baseband from the rtx status",
          "[dmr][opmode]")
{
    resetFakes();
    OpMode_DMR mode;
    mode.enable();
    rtxStatus_t s = channel(443700000, 443700000);
    s.dmrTimeslot = 1;

    mode.update(&s, false);
    REQUIRE(inits == 1);
    REQUIRE(lastConfig.access == DMR_ACCESS_DIRECT);
    REQUIRE(lastConfig.colorCode == 1);
    REQUIRE(lastConfig.timeslot == 2);
    REQUIRE(lastConfig.ownId == 3101003);
    REQUIRE(lastConfig.rxPromiscuous);

    // Only a new configuration reaches the running baseband
    mode.update(&s, false);
    REQUIRE(configures == 0);
    s.dmrColorCode = 3;
    mode.update(&s, true);
    REQUIRE(configures == 1);
    REQUIRE(lastConfig.colorCode == 3);
    REQUIRE(inits == 1);
}

TEST_CASE("DMR opmode takes a split of more than 10kHz as a repeater",
          "[dmr][opmode]")
{
    struct Case {
        freq_t rx, tx;
        enum dmrAccess access;
    };
    const Case cases[] = {
        { 443700000, 443700000, DMR_ACCESS_DIRECT },
        { 443700000, 443710000, DMR_ACCESS_DIRECT },
        { 443700000, 443710001, DMR_ACCESS_REPEATER },
        { 443710001, 443700000, DMR_ACCESS_REPEATER },
        { 443700000, 448700000, DMR_ACCESS_REPEATER },
    };

    for (const Case &c : cases) {
        resetFakes();
        OpMode_DMR mode;
        mode.enable();
        rtxStatus_t s = channel(c.rx, c.tx);
        mode.update(&s, false);
        REQUIRE(lastConfig.access == c.access);
    }
}

TEST_CASE("DMR opmode keys group and private calls", "[dmr][opmode]")
{
    for (int priv = 0; priv < 2; priv++) {
        resetFakes();
        OpMode_DMR mode;
        mode.enable();
        rtxStatus_t s = channel(443700000, 443700000);
        s.dmrPrivateCall = priv;
        s.dmrTalkgroup = priv ? 9990 : 91;
        run(mode, s);

        ptt = true;
        run(mode, s);
        REQUIRE(startGroup == (priv == 0));
        REQUIRE(startDst == (priv ? 9990u : 91u));
    }
}

TEST_CASE("DMR opmode switches the radio off on every handover",
          "[dmr][opmode]")
{
    resetFakes();
    OpMode_DMR mode;
    mode.enable();
    rtxStatus_t s = channel(443700000, 443700000);

    run(mode, s); // Into reception
    REQUIRE(s.opStatus == RX);

    ptt = true; // Into transmission
    run(mode, s);
    REQUIRE(s.opStatus == TX);

    ptt = false; // The baseband sends its terminator, then back to reception
    baseband_tx = false;
    run(mode, s);
    REQUIRE(s.opStatus == RX);

    const std::vector<std::string> expected = {
        "disableRtx", "enableRx", "disableRtx", "micOn",   "enableTx",
        "startTx",    "micOff",   "disableRtx", "enableRx"
    };
    REQUIRE(calls == expected);
}

TEST_CASE("DMR opmode does not transmit without a DMR ID", "[dmr][opmode]")
{
    resetFakes();
    OpMode_DMR mode;
    mode.enable();
    rtxStatus_t s = channel(443700000, 443700000);
    s.dmrId = 0;
    run(mode, s);

    ptt = true;
    run(mode, s, 3);
    for (const std::string &c : calls) {
        REQUIRE(c != "enableTx");
        REQUIRE(c != "startTx");
        REQUIRE(c != "micOn");
    }
}

TEST_CASE("DMR opmode transmits only once the baseband is ready to",
          "[dmr][opmode]")
{
    resetFakes();
    OpMode_DMR mode;
    mode.enable();
    rtxStatus_t s = channel(443700000, 448700000);
    run(mode, s);

    // Not ready, as through a repeater whose timing is not known
    readiness = DMR_TX_UNABLE;
    ptt = true;
    run(mode, s, 3);
    for (const std::string &c : calls) {
        REQUIRE(c != "enableTx");
        REQUIRE(c != "startTx");
        REQUIRE(c != "micOn");
    }

    // Ready on the next press
    ptt = false;
    run(mode, s, 2);
    readiness = DMR_TX_READY;
    ptt = true;
    run(mode, s, 3);
    REQUIRE(s.opStatus == TX);
    REQUIRE(std::count(calls.begin(), calls.end(), "startTx") == 1);
}

TEST_CASE("DMR opmode refuses a key-up once per press of the PTT",
          "[dmr][opmode]")
{
    resetFakes();
    OpMode_DMR mode;
    mode.enable();
    rtxStatus_t s = channel(443700000, 448700000);
    run(mode, s);
    readiness = DMR_TX_UNABLE;

    // Held down: refused once, then left alone
    ptt = true;
    run(mode, s, 20);
    REQUIRE(refusals == 1);
    REQUIRE(s.opStatus == RX);

    // Released and pressed again: refused again
    ptt = false;
    run(mode, s, 2);
    ptt = true;
    run(mode, s, 5);
    REQUIRE(refusals == 2);
}

TEST_CASE("DMR opmode plays the call being received, unless it is encrypted",
          "[dmr][opmode]")
{
    resetFakes();
    OpMode_DMR mode;
    mode.enable();
    rtxStatus_t s = channel(443700000, 443700000);
    run(mode, s);
    calls.clear();

    // A call comes in and is played until it is over
    basebandRx.valid = true;
    basebandRx.srcId = 3101003;
    run(mode, s);
    REQUIRE(calls == std::vector<std::string>({ "playOn" }));
    REQUIRE(s.dmrLcValid == 1);
    basebandRx.valid = false;
    run(mode, s);
    REQUIRE(calls == std::vector<std::string>({ "playOn", "playOff" }));

    // An encrypted one is shown but not played
    calls.clear();
    basebandRx.valid = true;
    basebandRx.encrypted = true;
    run(mode, s);
    REQUIRE(s.dmrLcValid == 1);
    REQUIRE(calls.empty());
}

TEST_CASE("DMR opmode asks for playback until the speaker is free",
          "[dmr][opmode]")
{
    resetFakes();
    OpMode_DMR mode;
    mode.enable();
    rtxStatus_t s = channel(443700000, 443700000);
    run(mode, s);

    speakerBusy = true; // A voice prompt holds the speaker
    basebandRx.valid = true;
    run(mode, s, 3);
    REQUIRE(playbackAsks == 3);
    REQUIRE_FALSE(playing);

    speakerBusy = false;
    run(mode, s, 1);
    REQUIRE(playing);
}

TEST_CASE("DMR opmode stops a playback that ended on its own", "[dmr][opmode]")
{
    resetFakes();
    OpMode_DMR mode;
    mode.enable();
    rtxStatus_t s = channel(443700000, 443700000);
    basebandRx.valid = true;
    run(mode, s);
    REQUIRE(playing);

    // A voice prompt takes the speaker over: playback ends on its own, and is
    // asked for again while the call lasts
    playing = false;
    speakerBusy = true;
    int asks = playbackAsks;
    run(mode, s, 2);
    REQUIRE(playbackAsks == asks + 2);

    // The call ends: what that playback left behind is released
    playbackStops = 0;
    basebandRx.valid = false;
    run(mode, s, 1);
    REQUIRE(playbackStops == 1);
}

TEST_CASE("DMR opmode runs the microphone for its transmission only",
          "[dmr][opmode]")
{
    resetFakes();
    OpMode_DMR mode;
    mode.enable();
    rtxStatus_t s = channel(443700000, 443700000);
    basebandRx.valid = true; // A call is being played when the PTT goes down
    run(mode, s);
    REQUIRE(playing);
    calls.clear();

    // The call is no longer played; the microphone starts before the key-up
    ptt = true;
    run(mode, s);
    const std::vector<std::string> keyed = { "playOff", "disableRtx", "micOn",
                                             "enableTx", "startTx" };
    REQUIRE(calls == keyed);

    // The microphone runs until the baseband is done with the terminator
    ptt = false;
    run(mode, s, 1);
    REQUIRE(capturing);
    baseband_tx = false;
    basebandRx.valid = false;
    run(mode, s);
    REQUIRE_FALSE(capturing);
    REQUIRE(s.opStatus == RX);
}

TEST_CASE("DMR opmode stops the audio after the baseband when disabled",
          "[dmr][opmode]")
{
    resetFakes();
    OpMode_DMR mode;
    mode.enable();
    rtxStatus_t s = channel(443700000, 443700000);
    basebandRx.valid = true;
    run(mode, s);
    calls.clear();
    mode.disable();
    REQUIRE(
        calls
        == std::vector<std::string>({ "terminate", "playOff", "disableRtx" }));

    resetFakes();
    mode.enable();
    s = channel(443700000, 443700000);
    run(mode, s);
    ptt = true;
    run(mode, s);
    calls.clear();
    mode.disable();
    REQUIRE(
        calls
        == std::vector<std::string>({ "terminate", "micOff", "disableRtx" }));
}

static int count(const std::string &call)
{
    return std::count(calls.begin(), calls.end(), call);
}

TEST_CASE("DMR opmode wakes the repeater up before keying up",
          "[dmr][opmode][repeater]")
{
    resetFakes();
    OpMode_DMR mode;
    mode.enable();
    rtxStatus_t s = channel(443700000, 448700000);
    run(mode, s);

    // The wake-up goes out with the radio transmitting, and in full
    readiness = DMR_TX_WAKEUP;
    wakeupHeld = true;
    ptt = true;
    calls.clear();
    run(mode, s, 3);
    REQUIRE(calls
            == std::vector<std::string>(
                { "disableRtx", "disableRtx", "enableTx", "wakeup" }));

    // Then the radio listens for the repeater
    baseband_tx = false;
    calls.clear();
    run(mode, s, 3);
    REQUIRE(calls == std::vector<std::string>({ "disableRtx", "enableRx" }));

    // The repeater answers: key up
    readiness = DMR_TX_READY;
    calls.clear();
    run(mode, s, 1);
    REQUIRE(calls
            == std::vector<std::string>(
                { "disableRtx", "micOn", "enableTx", "startTx" }));
    REQUIRE(s.opStatus == TX);
    REQUIRE(refusals == 0);
}

TEST_CASE("DMR opmode gives up on a repeater after four wake-ups",
          "[dmr][opmode][repeater]")
{
    resetFakes();
    OpMode_DMR mode;
    mode.enable();
    rtxStatus_t s = channel(443700000, 448700000);
    run(mode, s);

    readiness = DMR_TX_WAKEUP;
    ptt = true;
    run(mode, s, 3);
    REQUIRE(count("wakeup") == 1);

    // Sent again after 600ms without an answer
    long long first = clockMs;
    while ((count("wakeup") < 2) && (clockMs - first < 2000))
        run(mode, s, 1);
    REQUIRE(count("wakeup") == 2);
    REQUIRE(clockMs - first >= 600);
    REQUIRE(clockMs - first < 650);

    // Four in all, then refused, once while the PTT stays down
    run(mode, s, 400);
    REQUIRE(count("wakeup") == 4);
    REQUIRE(refusals == 1);
    REQUIRE(count("startTx") == 0);
    REQUIRE(s.opStatus == RX);
}

TEST_CASE("DMR opmode stops waking the repeater up when the PTT is released",
          "[dmr][opmode][repeater]")
{
    resetFakes();
    OpMode_DMR mode;
    mode.enable();
    rtxStatus_t s = channel(443700000, 448700000);
    run(mode, s);

    readiness = DMR_TX_WAKEUP;
    ptt = true;
    run(mode, s, 3);

    ptt = false;
    run(mode, s, 3);
    REQUIRE(s.opStatus == RX);
    REQUIRE(count("wakeup") == 1);
    REQUIRE(count("startTx") == 0);
    REQUIRE(refusals == 0);
    REQUIRE(calls.back() == "enableRx");
}

TEST_CASE("DMR opmode waits for the wake-up to go out before letting go",
          "[dmr][opmode][repeater]")
{
    resetFakes();
    OpMode_DMR mode;
    mode.enable();
    rtxStatus_t s = channel(443700000, 448700000);
    run(mode, s);

    readiness = DMR_TX_WAKEUP;
    wakeupHeld = true;
    ptt = true;
    run(mode, s, 3);

    // Released while the burst is still going out: the radio stays on air
    ptt = false;
    calls.clear();
    run(mode, s, 3);
    REQUIRE(calls.empty());
    REQUIRE(s.opStatus == TX);

    baseband_tx = false;
    run(mode, s, 3);
    REQUIRE(s.opStatus == RX);
}
