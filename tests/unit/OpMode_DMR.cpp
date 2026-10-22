/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The DMR operating mode against fakes of the radio, the platform and the
 * DMR baseband, which record what it asks of them: how it configures the
 * baseband from the rtx status, and how it hands over between reception and
 * transmission.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "interfaces/dmr.h"
#include "core/dmr_debug.h"
#include "rtx/OpMode_DMR.hpp"

/* ---------------------------------------------------------------- the fakes */

static std::vector<std::string> calls; // Radio and baseband calls, in order
static bool ptt;                       // PTT pressed
static bool baseband_tx;               // dmr_txActive
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
    (void)seconds;
    (void)mseconds;
}

void dmrDebug_log(const enum dmrDebugChannel channel, const char *fmt, ...)
{
    (void)channel;
    (void)fmt;
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
    memset(rx, 0, sizeof(*rx));
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

bool dmr_txActive()
{
    return baseband_tx;
}

} // extern "C"

/* ---------------------------------------------------------------- scenarios */

static void resetFakes()
{
    calls.clear();
    ptt = false;
    baseband_tx = false;
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

    const std::vector<std::string> expected = { "disableRtx", "enableRx",
                                                "disableRtx", "enableTx",
                                                "startTx",    "disableRtx",
                                                "enableRx" };
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
    }
}
