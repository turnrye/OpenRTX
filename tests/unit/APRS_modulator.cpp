/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Software loopback for the AFSK1200 transmit chain: encode and modulate a
 * frame, feed the baseband back into the demodulator, and check that the
 * frame that comes out is the one that went in. Bit stuffing, NRZI polarity,
 * bit order, the FCS and continuous-phase tones are each easy to get wrong
 * and invisible until a frame fails to decode; a round trip pins them all.
 */

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/utils.h"
#include "protocols/APRS/Demodulator.hpp"
#include "protocols/APRS/FrameEncoder.hpp"
#include "protocols/APRS/Modulator.hpp"
#include "protocols/APRS/packet.h"

/**
 * A frame as the decoder hands one back: raw bytes and a length.
 */
struct frameData {
    uint8_t data[APRS_PACLEN]; ///< Frame bytes.
    ssize_t len;               ///< Frame length.
};

/**
 * Transmit to receive rate ratio. The tones top out at 2200 Hz, well below
 * the 4800 Hz Nyquist limit at the receive rate, so a plain sample drop does
 * not alias.
 */
static constexpr size_t DECIMATION = APRS::Modulator::TX_SAMPLE_RATE
                                   / APRS_SAMPLE_RATE;

static_assert(APRS::Modulator::TX_SAMPLE_RATE % APRS_SAMPLE_RATE == 0,
              "transmit rate must be a whole multiple of the receive rate");

/**
 * Peak level fed to the demodulator. Its correlators saturate above about
 * 500 peak, far below transmit level, so the loopback rescales.
 */
static constexpr int32_t RX_PEAK = 300;

static constexpr size_t LEAD_FLAGS = 12;   ///< Opening flags for the loopback.
static constexpr size_t TAIL_FLAGS = 3;    ///< Closing flags for the loopback.
static constexpr uint32_t SPACE_HZ = 2200; ///< Higher of the two tones.

/**
 * Build a frame from a source, path and info field.
 */
static frameData buildFrame(const char *path, const char *info)
{
    frameData frame;

    frame.len = aprsFrame_build(frame.data, sizeof(frame.data), APRS_TOCALL,
                                "N0CALL-7", path, info, strlen(info));
    REQUIRE(frame.len > 0);

    return frame;
}

/**
 * Encode and modulate frames one after another, as back-to-back
 * transmissions on one modulator, and return the baseband.
 */
static std::vector<int16_t> modulate(const std::vector<frameData> &frames)
{
    APRS::FrameEncoder encoder;
    APRS::Modulator mod;
    std::vector<int16_t> baseband;
    stream_sample_t block[APRS_BUF_SIZE];

    for (const frameData &frame : frames) {
        encoder.start(frame.data, (size_t)frame.len, LEAD_FLAGS, TAIL_FLAGS);

        size_t n;
        do {
            n = mod.generate(encoder, block, ARRAY_SIZE(block));
            baseband.insert(baseband.end(), block, block + n);
        } while (n == ARRAY_SIZE(block));
    }

    return baseband;
}

/**
 * Resample and rescale transmit baseband for the demodulator.
 */
static std::vector<int16_t> toReceiveBaseband(const std::vector<int16_t> &tx)
{
    int32_t peak = 1;
    for (int16_t s : tx) {
        if (std::abs((int32_t)s) > peak)
            peak = std::abs((int32_t)s);
    }

    std::vector<int16_t> rx;
    rx.reserve(tx.size() / DECIMATION);
    for (size_t i = 0; i < tx.size(); i += DECIMATION)
        rx.push_back((int16_t)(((int32_t)tx[i] * RX_PEAK) / peak));

    return rx;
}

/**
 * Demodulate baseband in stream-sized blocks and return every frame decoded.
 */
static std::vector<frameData> demodulate(std::vector<int16_t> &rx)
{
    APRS::Demodulator demod;
    std::vector<frameData> frames;

    demod.init();

    for (size_t i = 0; i < rx.size(); i += APRS_BUF_SIZE) {
        const size_t left = rx.size() - i;
        dataBlock_t block;

        block.data = &rx[i];
        block.len = (left < APRS_BUF_SIZE) ? left : APRS_BUF_SIZE;
        if (!demod.update(block))
            continue;

        frameData f;
        f.len = demod.getFrame(f.data, sizeof(f.data));
        if (f.len > 0)
            frames.push_back(f);
    }

    return frames;
}

/**
 * Modulate frames and demodulate them back.
 */
static std::vector<frameData> loopback(const std::vector<frameData> &tx)
{
    std::vector<int16_t> rx = toReceiveBaseband(modulate(tx));
    return demodulate(rx);
}

/**
 * Number of symbols the encoder produces for a frame.
 */
static size_t countSymbols(const frameData &frame, size_t leadFlags,
                           size_t tailFlags)
{
    APRS::FrameEncoder encoder;
    size_t count = 0;
    uint8_t tone;

    encoder.start(frame.data, (size_t)frame.len, leadFlags, tailFlags);
    while (encoder.nextSymbol(tone))
        count++;

    return count;
}

TEST_CASE("APRS modulator: a modulated frame demodulates back", "[aprs]")
{
    const frameData tx = buildFrame(APRS_DEFAULT_PATH,
                                    ":W1AW     :hello there");

    std::vector<frameData> frames = loopback({ tx });

    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].len == tx.len);
    REQUIRE(memcmp(frames[0].data, tx.data, (size_t)tx.len) == 0);
}

TEST_CASE("APRS modulator: the round trip preserves the whole packet", "[aprs]")
{
    char info[APRS_MSG_ADDRESSEE_LEN + APRS_MSG_TEXT_MAX + 3];
    REQUIRE(aprsMsg_format(info, sizeof(info), "W1AW-9", "meet on 146.52") > 0);

    std::vector<frameData> frames =
        loopback({ buildFrame(APRS_DEFAULT_PATH, info) });
    REQUIRE(frames.size() == 1);

    struct aprsPacket pkt;
    REQUIRE(aprsPkt_fromFrame(frames[0].data, (size_t)frames[0].len, &pkt)
            == 0);

    char addr[APRS_ADDR_STR_LEN];
    aprsAddr_toStr(&pkt.addresses[0], addr, sizeof(addr));
    REQUIRE(strcmp(addr, APRS_TOCALL) == 0);
    aprsAddr_toStr(&pkt.addresses[1], addr, sizeof(addr));
    REQUIRE(strcmp(addr, "N0CALL-7") == 0);

    char path[32];
    aprsPath_toStr(&pkt, path, sizeof(path));
    REQUIRE(strcmp(path, APRS_DEFAULT_PATH) == 0);

    char addressee[APRS_ADDR_STR_LEN];
    char text[APRS_PACLEN];
    REQUIRE(pkt.type == APRS_TYPE_MESSAGE);
    REQUIRE(
        aprsMsg_unwrap(&pkt, addressee, sizeof(addressee), text, sizeof(text))
        == 0);
    REQUIRE(strcmp(addressee, "W1AW-9") == 0);
    REQUIRE(strcmp(text, "meet on 146.52") == 0);
}

TEST_CASE("APRS modulator: bit stuffing survives data that looks like a flag",
          "[aprs]")
{
    // '~' is the flag byte 0x7e: unstuffed, it would end the frame early
    const frameData tx = buildFrame(nullptr, ":W1AW     :~~~~~~~~ and back");

    std::vector<frameData> frames = loopback({ tx });

    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].len == tx.len);
    REQUIRE(memcmp(frames[0].data, tx.data, (size_t)tx.len) == 0);
}

TEST_CASE("APRS modulator: back-to-back frames both decode", "[aprs]")
{
    const frameData first = buildFrame(APRS_DEFAULT_PATH, ":W1AW     :first");
    const frameData second = buildFrame(APRS_DEFAULT_PATH, ":W1AW     :second");

    std::vector<frameData> frames = loopback({ first, second });

    REQUIRE(frames.size() == 2);
    REQUIRE(frames[0].len == first.len);
    REQUIRE(memcmp(frames[0].data, first.data, (size_t)first.len) == 0);
    REQUIRE(frames[1].len == second.len);
    REQUIRE(memcmp(frames[1].data, second.data, (size_t)second.len) == 0);
}

TEST_CASE("APRS encoder: each flag asked for is eight symbols", "[aprs]")
{
    const frameData tx = buildFrame(APRS_DEFAULT_PATH, ">flags");
    const size_t base = countSymbols(tx, 1, 1);

    REQUIRE(countSymbols(tx, 45, 1) == (base + (44 * CHAR_BIT)));
    REQUIRE(countSymbols(tx, 1, 4) == (base + (3 * CHAR_BIT)));
}

TEST_CASE("APRS modulator: the tone is continuous", "[aprs]")
{
    const std::vector<int16_t> tx = modulate(
        { buildFrame(APRS_DEFAULT_PATH, ":W1AW     :~~ phase check ~~") });

    // A sine at amplitude A and frequency f changes by at most 2 pi f A / fs
    // per sample; a phase jump between symbols shows up as more
    const double slope = 2.0 * M_PI * SPACE_HZ * APRS::Modulator::TX_AMPLITUDE
                       / APRS::Modulator::TX_SAMPLE_RATE;
    const int32_t maxStep = (int32_t)(slope * 1.1);

    int32_t worst = 0;
    for (size_t i = 1; i < tx.size(); i++) {
        const int32_t delta = std::abs((int32_t)tx[i] - (int32_t)tx[i - 1]);
        if (delta > worst)
            worst = delta;
    }

    REQUIRE(worst <= maxStep);
}

TEST_CASE("APRS modulator: transmit level stays inside the sample range",
          "[aprs]")
{
    const std::vector<int16_t> tx =
        modulate({ buildFrame(APRS_DEFAULT_PATH, ":W1AW     :level check") });

    int32_t peak = 0;
    for (int16_t s : tx) {
        if (std::abs((int32_t)s) > peak)
            peak = std::abs((int32_t)s);
    }

    // Under-driving leaves a signal nobody decodes; over-driving splatters
    REQUIRE(peak <= APRS::Modulator::TX_AMPLITUDE);
    REQUIRE(peak > ((APRS::Modulator::TX_AMPLITUDE * 9) / 10));
}
