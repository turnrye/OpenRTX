/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>
#include "core/dsp.h"

using Catch::Matchers::WithinAbs;

static constexpr float FS = 8000.0f;
static constexpr size_t BLK = 160;

static float rmsDb(const float *x, size_t n)
{
    double p = 0.0;
    for (size_t i = 0; i < n; i++)
        p += (double)x[i] * x[i];
    return (float)(10.0 * std::log10(p / (double)n + 1e-12));
}

// Run a sine of the given level (dBFS RMS) through the AGC for a number of
// blocks; return the RMS of the last block and its peak.
static void runTone(struct agcState *agc, float levelDb, float hz,
                    size_t blocks, float *lastRmsDb, float *peak, size_t *phase)
{
    const float amp = std::sqrt(2.0f) * std::pow(10.0f, levelDb / 20.0f);
    float buf[BLK];
    *peak = 0.0f;
    for (size_t b = 0; b < blocks; b++) {
        for (size_t i = 0; i < BLK; i++, (*phase)++)
            buf[i] = amp
                   * std::sin(2.0f * (float)M_PI * hz * (float)(*phase) / FS);
        dsp_agcProcess(agc, buf, BLK);
        for (size_t i = 0; i < BLK; i++)
            *peak = std::max(*peak, std::fabs(buf[i]));
    }
    *lastRmsDb = rmsDb(buf, BLK);
}

TEST_CASE("High-pass passes 1 kHz and rejects 50 Hz", "[dsp][hpf]")
{
    for (float hz : { 50.0f, 1000.0f }) {
        struct biquad bq;
        dsp_highPassInit(&bq, 110.0f, FS);
        std::vector<float> y(8000);
        for (size_t i = 0; i < y.size(); i++)
            y[i] = dsp_biquad(
                &bq, std::sin(2.0f * (float)M_PI * hz * (float)i / FS));
        // Gain relative to the input: a unit sine is -3.01 dBFS RMS
        const float db = rmsDb(y.data() + 4000, 4000) + 3.0103f;
        if (hz == 1000.0f)
            REQUIRE_THAT(db, WithinAbs(0.0, 0.2));
        else
            REQUIRE(db < -12.0f);
    }
}

TEST_CASE("AGC brings steady tones of different levels to the target",
          "[dsp][agc]")
{
    // +10 dBFS: the float chain applies the base gain unclipped, so loud
    // input reaches the AGC well above full scale
    for (float level : { -45.0f, -33.0f, -21.0f, -12.0f, 10.0f }) {
        struct agcParams p;
        dsp_agcDefaults(&p);
        struct agcState agc;
        dsp_agcInit(&agc, &p);

        float out, peak;
        size_t phase = 0;
        runTone(&agc, level, 440.0f, 600, &out, &peak, &phase); // 12 s
        REQUIRE_THAT(out, WithinAbs(p.targetDb, 1.5));
    }
}

TEST_CASE("Limiter keeps every sample under the ceiling", "[dsp][agc]")
{
    struct agcParams p;
    dsp_agcDefaults(&p);
    struct agcState agc;
    dsp_agcInit(&agc, &p);

    float out, peak;
    size_t phase = 0;
    // A sudden full-scale burst after the AGC settled on a quiet talker
    runTone(&agc, -40.0f, 300.0f, 400, &out, &peak, &phase);
    runTone(&agc, 0.0f, 300.0f, 20, &out, &peak, &phase);
    REQUIRE(peak <= std::pow(10.0f, p.ceilingDb / 20.0f) + 1e-6f);
}

TEST_CASE("Gate holds the gain through silence", "[dsp][agc]")
{
    struct agcParams p;
    dsp_agcDefaults(&p);
    struct agcState agc;
    dsp_agcInit(&agc, &p);

    float out, peak;
    size_t phase = 0;
    runTone(&agc, -35.0f, 440.0f, 500, &out, &peak, &phase);
    const float settled = agc.gainDb;

    // Five seconds of low background noise (pseudo-random, -70 dBFS)
    float buf[BLK];
    uint32_t seed = 1;
    for (size_t b = 0; b < 250; b++) {
        for (size_t i = 0; i < BLK; i++) {
            seed = seed * 1664525u + 1013904223u;
            buf[i] = 3e-4f * ((float)(seed >> 8) / 8388608.0f - 1.0f);
        }
        dsp_agcProcess(&agc, buf, BLK);
    }

    REQUIRE(agc.gateOpen == false);
    REQUIRE_THAT(agc.gainDb, WithinAbs(settled, 0.01));
}

TEST_CASE("AGC on digital silence produces silence, not NaN", "[dsp][agc]")
{
    struct agcParams p;
    dsp_agcDefaults(&p);
    struct agcState agc;
    dsp_agcInit(&agc, &p);

    float buf[BLK] = { 0 };
    for (size_t b = 0; b < 100; b++)
        dsp_agcProcess(&agc, buf, BLK);

    for (size_t i = 0; i < BLK; i++)
        REQUIRE(buf[i] == 0.0f);
    REQUIRE(std::isfinite(agc.gainDb));
}

TEST_CASE("Expander attenuates a pause only after the hangover",
          "[dsp][agc][expander]")
{
    struct agcParams p;
    dsp_agcDefaults(&p);
    p.expanderDb = 20.0f;
    struct agcState agc;
    dsp_agcInit(&agc, &p);

    float out, peak;
    size_t phase = 0;
    runTone(&agc, -30.0f, 440.0f, 300, &out, &peak, &phase);
    const float gain = agc.gainDb;

    // Low noise: a short gap below the hangover is untouched...
    float buf[BLK];
    uint32_t seed = 7;
    auto noise = [&](size_t blocks) {
        for (size_t b = 0; b < blocks; b++) {
            for (size_t i = 0; i < BLK; i++) {
                seed = seed * 1664525u + 1013904223u;
                buf[i] = 3e-4f * ((float)(seed >> 8) / 8388608.0f - 1.0f);
            }
            dsp_agcProcess(&agc, buf, BLK);
        }
    };
    noise(p.hangoverBlocks - 1);
    REQUIRE(dsp_agcSilent(&agc) == false);
    REQUIRE(agc.expDb == 0.0f);

    // ...a long pause fades to the full expander depth, gain still held
    noise(40);
    REQUIRE(dsp_agcSilent(&agc) == true);
    REQUIRE_THAT(agc.expDb, WithinAbs(-p.expanderDb, 0.01));
    REQUIRE_THAT(agc.gainDb, WithinAbs(gain, 0.01));

    // Speech returns: attenuation is gone within one block
    runTone(&agc, -30.0f, 440.0f, 1, &out, &peak, &phase);
    REQUIRE(agc.expDb == 0.0f);
    REQUIRE(dsp_agcSilent(&agc) == false);
}

/**
 * Run uniform noise of the given RMS level through the AGC.
 *
 * @param agc: AGC state.
 * @param levelDb: noise level, dBFS RMS.
 * @param blocks: number of blocks.
 * @param seed: generator state, updated.
 */
static void runNoise(struct agcState *agc, float levelDb, size_t blocks,
                     uint32_t *seed)
{
    const float amp = std::sqrt(3.0f) * std::pow(10.0f, levelDb / 20.0f);
    float buf[BLK];
    for (size_t b = 0; b < blocks; b++) {
        for (size_t i = 0; i < BLK; i++) {
            *seed = *seed * 1664525u + 1013904223u;
            buf[i] = amp * ((float)(*seed >> 8) / 8388608.0f - 1.0f);
        }
        dsp_agcProcess(agc, buf, BLK);
    }
}

TEST_CASE("Windowed floor locks onto a loud noise bed", "[dsp][agc][agc2]")
{
    struct agcParams p;
    dsp_agcDefaults(&p);
    p.floorWindowBlocks = 20;
    struct agcState agc;
    dsp_agcInit(&agc, &p);

    float out, peak;
    size_t phase = 0;
    uint32_t seed = 3;
    runTone(&agc, -20.0f, 440.0f, 50, &out, &peak, &phase);
    runNoise(&agc, -40.0f, 100, &seed);

    REQUIRE(agc.floorDb > -45.0f);
    REQUIRE(agc.floorDb < -39.0f);
    REQUIRE(agc.gateOpen == false);
}

TEST_CASE("Short noise bursts do not raise the gain", "[dsp][agc][agc2]")
{
    // Settle on a talker, then 10 s of quiet noise with 80 ms bursts
    auto drift = [](uint16_t confirm) {
        struct agcParams p;
        dsp_agcDefaults(&p);
        p.floorWindowBlocks = 20;
        p.confirmBlocks = confirm;
        struct agcState agc;
        dsp_agcInit(&agc, &p);

        float out, peak;
        size_t phase = 0;
        uint32_t seed = 5;
        runTone(&agc, -20.0f, 440.0f, 250, &out, &peak, &phase);
        const float settled = agc.gainDb;
        for (size_t i = 0; i < 20; i++) {
            runNoise(&agc, -50.0f, 21, &seed);
            runNoise(&agc, -32.0f, 4, &seed);
        }
        return agc.gainDb - settled;
    };

    REQUIRE(drift(0) > 2.0f); // without confirmation the bursts pump
    REQUIRE(std::fabs(drift(6)) < 0.5f);
}

TEST_CASE("No silence while speech is close to the floor", "[dsp][agc][agc2]")
{
    // A talker over noise: words of 300 ms, pauses of 400 ms
    auto silent = [](float noiseDb) {
        struct agcParams p;
        dsp_agcDefaults(&p);
        p.floorWindowBlocks = 20;
        p.confirmBlocks = 6;
        p.gateAboveFloor = 5.0f;
        p.silenceSnrDb = 15.0f;
        struct agcState agc;
        dsp_agcInit(&agc, &p);

        float out, peak;
        size_t phase = 0;
        uint32_t seed = 9;
        for (size_t i = 0; i < 20; i++) {
            runTone(&agc, -20.0f, 440.0f, 15, &out, &peak, &phase);
            runNoise(&agc, noiseDb, 20, &seed);
        }
        return dsp_agcSilent(&agc);
    };

    REQUIRE(silent(-50.0f) == true);
    REQUIRE(silent(-28.0f) == false);
}
