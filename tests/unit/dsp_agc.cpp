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

static constexpr float FS  = 8000.0f;
static constexpr size_t BLK = 160;

static float rmsDb(const float *x, size_t n)
{
    double p = 0.0;
    for(size_t i = 0; i < n; i++)
        p += (double)x[i] * x[i];
    return (float)(10.0 * std::log10(p / (double)n + 1e-12));
}

// Run a sine of the given level (dBFS RMS) through the AGC for a number of
// blocks; return the RMS of the last block and its peak.
static void runTone(struct agcState *agc, float levelDb, float hz, size_t blocks,
                    float *lastRmsDb, float *peak, size_t *phase)
{
    const float amp = std::sqrt(2.0f) * std::pow(10.0f, levelDb / 20.0f);
    float buf[BLK];
    *peak = 0.0f;
    for(size_t b = 0; b < blocks; b++) {
        for(size_t i = 0; i < BLK; i++, (*phase)++)
            buf[i] = amp * std::sin(2.0f * (float)M_PI * hz * (float)(*phase) / FS);
        dsp_agcProcess(agc, buf, BLK);
        for(size_t i = 0; i < BLK; i++)
            *peak = std::max(*peak, std::fabs(buf[i]));
    }
    *lastRmsDb = rmsDb(buf, BLK);
}

TEST_CASE("High-pass passes 1 kHz and rejects 50 Hz", "[dsp][hpf]")
{
    for(float hz : {50.0f, 1000.0f}) {
        struct biquad bq;
        dsp_highPassInit(&bq, 110.0f, FS);
        std::vector<float> y(8000);
        for(size_t i = 0; i < y.size(); i++)
            y[i] = dsp_biquad(&bq, std::sin(2.0f * (float)M_PI * hz * (float)i / FS));
        // Gain relative to the input: a unit sine is -3.01 dBFS RMS
        const float db = rmsDb(y.data() + 4000, 4000) + 3.0103f;
        if(hz == 1000.0f)
            REQUIRE_THAT(db, WithinAbs(0.0, 0.2));
        else
            REQUIRE(db < -12.0f);
    }
}

TEST_CASE("AGC brings steady tones of different levels to the target",
          "[dsp][agc]")
{
    for(float level : {-45.0f, -33.0f, -21.0f, -12.0f}) {
        struct agcParams p;
        dsp_agcDefaults(&p);
        struct agcState agc;
        dsp_agcInit(&agc, &p);

        float out, peak;
        size_t phase = 0;
        runTone(&agc, level, 440.0f, 600, &out, &peak, &phase);   // 12 s
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
    for(size_t b = 0; b < 250; b++) {
        for(size_t i = 0; i < BLK; i++) {
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

    float buf[BLK] = {0};
    for(size_t b = 0; b < 100; b++)
        dsp_agcProcess(&agc, buf, BLK);

    for(size_t i = 0; i < BLK; i++)
        REQUIRE(buf[i] == 0.0f);
    REQUIRE(std::isfinite(agc.gainDb));
}
