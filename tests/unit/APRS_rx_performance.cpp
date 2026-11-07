/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Receive performance of the AFSK1200 demodulator against synthesised
 * signals, the way the M17 demodulator is tested: a known set of packets
 * goes through an impairment, and the share that decodes is checked against
 * a floor. The floors are set a little under what the demodulator does
 * today, so a regression fails here and an improvement shows in the tables
 * each sweep prints.
 */

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "APRSSignal.hpp"
#include "NoisyChannel.hpp"
#include "protocols/APRS/Demodulator.hpp"
#include "protocols/APRS/constants.h"
#include "protocols/APRS/packet.h"

static constexpr size_t PACKETS = 60;
static constexpr double GAP = 0.1; ///< Seconds between packets.

using Frame = std::vector<uint8_t>;

static std::vector<Frame> testFrames()
{
    std::vector<Frame> frames;
    for (size_t i = 0; i < PACKETS; i++)
        frames.push_back(aprsTestFrame(i));
    return frames;
}

/** Every frame the demodulator completes, fed in stream-sized blocks. */
static std::vector<Frame> decode(std::vector<int16_t> rx)
{
    APRS::Demodulator demod;
    demod.init();

    std::vector<Frame> frames;
    for (size_t i = 0; i < rx.size(); i += APRS_BUF_SIZE) {
        dataBlock_t block;
        block.data = &rx[i];
        block.len = std::min<size_t>(APRS_BUF_SIZE, rx.size() - i);

        if (demod.update(block)) {
            Frame f(APRS_PACLEN);
            ssize_t n = demod.getFrame(f.data(), f.size());
            if (n > 0) {
                f.resize((size_t)n);
                frames.push_back(f);
            }
        }
    }

    demod.terminate();
    return frames;
}

/** How many of @p expected came out of the demodulator, each at most once. */
static size_t matched(const std::vector<Frame> &expected,
                      const std::vector<Frame> &decoded)
{
    std::vector<bool> seen(expected.size(), false);
    size_t count = 0;

    for (const Frame &d : decoded) {
        for (size_t i = 0; i < expected.size(); i++) {
            if (!seen[i] && (expected[i] == d)) {
                seen[i] = true;
                count++;
                break;
            }
        }
    }

    return count;
}

/**
 * Send every test frame through a channel with noise of @p noiseRms and
 * return the share decoded.
 */
static double decodedShare(const AprsSignalParams &params, float noiseRms,
                           uint32_t seed = 1, int16_t dcOffset = 0)
{
    std::vector<Frame> frames = testFrames();
    NoisyChannel channel(params.rate, seed, noiseRms, noiseRms);

    channel.idle(GAP);
    for (const Frame &f : frames) {
        channel.transmit(aprsSignal(f.data(), f.size(), params));
        channel.idle(GAP);
    }

    std::vector<int16_t> rx = channel.samples();
    if (dcOffset != 0) {
        for (int16_t &s : rx)
            s = (int16_t)std::min(32767, std::max(-32768, s + dcOffset));
    }

    return (double)matched(frames, decode(rx)) / (double)frames.size();
}

/** Noise rms giving @p snrDb against a sine of @p peak, over the whole band. */
static float noiseFor(double snrDb, double peak)
{
    const double signalRms = peak / std::sqrt(2.0);
    return (float)(signalRms / std::pow(10.0, snrDb / 20.0));
}

TEST_CASE("APRS rx: every clean packet decodes", "[aprs][rx]")
{
    AprsSignalParams params;
    std::vector<Frame> frames = testFrames();

    for (size_t i = 0; i < frames.size(); i++) {
        NoisyChannel channel(params.rate, 1, 0.0f, 0.0f);
        channel.idle(GAP);
        channel.transmit(
            aprsSignal(frames[i].data(), frames[i].size(), params));
        channel.idle(GAP);

        INFO("frame " << i << ", " << frames[i].size() << " bytes");
        CHECK(matched({ frames[i] }, decode(channel.samples())) == 1);
    }

    CHECK(decodedShare(params, 0.0f) == 1.0);
}

TEST_CASE("APRS rx: frame length up to the decoder's limit", "[aprs][rx]")
{
    /* The decoder keeps a frame and its FCS in APRS_PACLEN bytes, so the
     * longest frame it can hand back is two bytes shorter than the longest
     * the builder makes: a full 256-byte info field behind ten addresses. */
    AprsSignalParams params;
    const char prefix[] = "=4903.50N/07201.75W-";
    char info[APRS_MAX_INFO];
    memcpy(info, prefix, strlen(prefix));
    for (size_t i = strlen(prefix); i < sizeof(info); i++)
        info[i] = (char)('0' + (i % 10));

    auto decodes = [&](const char *path, size_t len) {
        Frame frame(APRS_PACLEN);
        ssize_t n = aprsFrame_build(frame.data(), frame.size(), "APORT1",
                                    "N2BP-7", path, info, len);
        REQUIRE(n > 0);
        frame.resize((size_t)n);

        NoisyChannel channel(params.rate, 1, 0.0f, 0.0f);
        channel.idle(GAP);
        channel.transmit(aprsSignal(frame.data(), frame.size(), params));
        channel.idle(GAP);

        const bool ok = matched({ frame }, decode(channel.samples())) == 1;
        return ok ? frame.size() : 0;
    };

    for (size_t len = 20; len <= 256; len += 9) {
        INFO("info length " << len);
        CHECK(decodes(nullptr, len) > 0);
    }

    const char *fullPath = "WIDE1-1,WIDE2-1,A1A,B2B,C3C,D4D,E5E,F6F";
    size_t longest = decodes(fullPath, 256);
    printf("APRS rx, longest frame decoded: %zu bytes (APRS_PACLEN %d)\n",
           longest, APRS_PACLEN);
    CHECK(longest == (APRS_PACLEN - APRS_FCS_LEN));
}

TEST_CASE("APRS rx: decoding against white noise", "[aprs][rx]")
{
    const double snrs[] = { 24, 18, 15, 12, 9, 6, 3, 0 };
    AprsSignalParams params;

    printf("APRS rx, white noise (full-band SNR at %u S/s)\n", params.rate);
    double at[8];
    for (size_t i = 0; i < 8; i++) {
        at[i] = decodedShare(params, noiseFor(snrs[i], params.markPeak));
        printf("  %5.1f dB  %5.1f %%\n", snrs[i], 100.0 * at[i]);
    }

    CHECK(at[0] == 1.0);  /* 24 dB */
    CHECK(at[1] == 1.0);  /* 18 dB */
    CHECK(at[4] >= 0.95); /* 9 dB */
    CHECK(at[5] >= 0.85); /* 6 dB */
}

TEST_CASE("APRS rx: tolerance to twist", "[aprs][rx]")
{
    /* Twist is the mark level over the space level, in dB; pre-emphasis and
     * de-emphasis mismatches between radios produce several dB of it. */
    const double twists[] = { -9, -6, -3, 0, 3, 6, 9 };

    printf("APRS rx, twist (mark/space), clean\n");
    double at[7];
    for (size_t i = 0; i < 7; i++) {
        AprsSignalParams params;
        params.markPeak = (float)(300.0 * std::pow(10.0, twists[i] / 40.0));
        params.spacePeak = (float)(300.0 * std::pow(10.0, -twists[i] / 40.0));
        at[i] = decodedShare(params, 0.0f);
        printf("  %+5.1f dB  %5.1f %%\n", twists[i], 100.0 * at[i]);
    }

    CHECK(at[3] == 1.0);  /* 0 dB */
    CHECK(at[1] >= 0.95); /* -6 dB */
    CHECK(at[5] >= 0.95); /* +6 dB */
}

TEST_CASE("APRS rx: input level", "[aprs][rx]")
{
    /* Nothing in the firmware sets the level reaching the demodulator, so
     * every level from weak to full scale must decode. */
    const double peaks[] = { 50,  100, 200,  300,  400, 500,
                             600, 800, 1200, 2000, 4000 };

    printf("APRS rx, peak input level, clean\n");
    double at[11];
    for (size_t i = 0; i < 11; i++) {
        AprsSignalParams params;
        params.markPeak = params.spacePeak = (float)peaks[i];
        at[i] = decodedShare(params, 0.0f);
        printf("  %6.0f  %5.1f %%\n", peaks[i], 100.0 * at[i]);
    }

    for (size_t i = 0; i < 11; i++)
        CHECK(at[i] == 1.0);
}

TEST_CASE("APRS rx: tolerance to the sender's clock error", "[aprs][rx]")
{
    const double errors[] = { -3, -2, -1, -0.5, 0, 0.5, 1, 2, 3 };

    printf("APRS rx, sender clock error, clean\n");
    double at[9];
    for (size_t i = 0; i < 9; i++) {
        AprsSignalParams params;
        params.clockError = (float)(errors[i] / 100.0);
        at[i] = decodedShare(params, 0.0f);
        printf("  %+5.1f %%  %5.1f %%\n", errors[i], 100.0 * at[i]);
    }

    CHECK(at[2] == 1.0);  /* -1 % */
    CHECK(at[6] == 1.0);  /* +1 % */
    CHECK(at[1] >= 0.95); /* -2 % */
    CHECK(at[7] >= 0.95); /* +2 % */
}

TEST_CASE("APRS rx: short preambles", "[aprs][rx]")
{
    /* Stations with a short TXDELAY give the receiver few flags to settle
     * on; 2 flags is about 13 ms. */
    const unsigned flags[] = { 2, 4, 8, 16, 32 };

    printf("APRS rx, opening flags, clean\n");
    double at[5];
    for (size_t i = 0; i < 5; i++) {
        AprsSignalParams params;
        params.flagsBefore = flags[i];
        params.flagsAfter = 1;
        at[i] = decodedShare(params, 0.0f);
        printf("  %2u flags  %5.1f %%\n", flags[i], 100.0 * at[i]);
    }

    CHECK(at[4] == 1.0);  /* 32 */
    CHECK(at[1] >= 0.95); /* 4 */
    CHECK(at[0] >= 0.90); /* 2 */
}

TEST_CASE("APRS rx: a DC offset is blocked", "[aprs][rx]")
{
    AprsSignalParams params;
    CHECK(decodedShare(params, 0.0f, 1, 3000) == 1.0);
    CHECK(decodedShare(params, 0.0f, 1, -3000) == 1.0);
}

TEST_CASE("APRS rx: noise alone decodes nothing", "[aprs][rx]")
{
    for (float rms : { 100.0f, 1000.0f, 8000.0f }) {
        NoisyChannel channel(9600, 7, rms, rms);
        channel.idle(20.0);
        CHECK(decode(channel.samples()).empty());
    }
}
