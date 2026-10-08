/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>
#include "core/mic_frame.h"

/* The Linux target sets no gate thresholds; set them on the state. */
static struct micFrameState gated()
{
    struct micFrameState st;
    micFrame_init(&st);
    st.openLevel = 1000;
    st.closeLevel = 500;
    return st;
}

static std::vector<int16_t> tone(int amplitude)
{
    std::vector<int16_t> f(MIC_FRAME_SAMPLES);
    for (size_t i = 0; i < f.size(); i++)
        f[i] = amplitude * std::sin(2 * M_PI * 1000.0 * i / 8000.0);
    return f;
}

TEST_CASE("Mic frame RMS", "[mic]")
{
    REQUIRE(micFrame_rms(tone(0).data(), MIC_FRAME_SAMPLES) == 0);
    uint16_t r = micFrame_rms(tone(10000).data(), MIC_FRAME_SAMPLES);
    REQUIRE(r > 7000); // 10000 / sqrt(2) = 7071
    REQUIRE(r < 7150);
}

TEST_CASE("Mic gate stays open without thresholds", "[mic]")
{
    struct micFrameState st;
    micFrame_init(&st);
    st.openLevel = 0;
    for (int n = 0; n < 50; n++)
        REQUIRE(micFrame_hasVoice(&st, tone(0).data(), MIC_FRAME_SAMPLES));
}

TEST_CASE("Mic gate opens on voice and closes after the hold", "[mic]")
{
    struct micFrameState st = gated();
    auto loud = tone(3000);
    auto quiet = tone(100);

    // Open at start, so nothing is lost before the first frame is judged
    REQUIRE(micFrame_hasVoice(&st, loud.data(), MIC_FRAME_SAMPLES));

    // Quiet frames: open for the hold, then closed
    int openFrames = 0;
    for (int n = 0; n < 40; n++) {
        if (micFrame_hasVoice(&st, quiet.data(), MIC_FRAME_SAMPLES))
            openFrames++;
    }
    REQUIRE(openFrames == MIC_GATE_HOLD_FRAMES);
    REQUIRE_FALSE(micFrame_hasVoice(&st, quiet.data(), MIC_FRAME_SAMPLES));

    // Voice opens it again at once
    REQUIRE(micFrame_hasVoice(&st, loud.data(), MIC_FRAME_SAMPLES));
}

TEST_CASE("Mic gate does not chatter between the thresholds", "[mic]")
{
    struct micFrameState st = gated();
    auto between = tone(1000); // RMS about 700: above close, below open

    // Closed: stays closed
    for (int n = 0; n < 20; n++)
        micFrame_hasVoice(&st, tone(0).data(), MIC_FRAME_SAMPLES);
    REQUIRE_FALSE(micFrame_hasVoice(&st, between.data(), MIC_FRAME_SAMPLES));

    // Open: stays open, indefinitely
    micFrame_hasVoice(&st, tone(3000).data(), MIC_FRAME_SAMPLES);
    for (int n = 0; n < 100; n++)
        REQUIRE(micFrame_hasVoice(&st, between.data(), MIC_FRAME_SAMPLES));
}

TEST_CASE("Mic frame conditioning keeps a frame of samples", "[mic]")
{
    struct micFrameState st;
    micFrame_init(&st);
    std::vector<int16_t> buf(MIC_FRAME_SAMPLES, 100);
    size_t n = micFrame_condition(&st, buf.data(), buf.size());
    REQUIRE(n == MIC_FRAME_SAMPLES); // Linux: no oversampling
}
