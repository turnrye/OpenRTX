/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Software loopback for the AFSK1200 transmit chain: modulate a frame, feed
 * the baseband straight back into the demodulator, and check that what comes
 * out the other end is the frame that went in.
 *
 * Running both halves in one process is what makes this worth having as a
 * unit test rather than only as an emulator script.  Bit stuffing, NRZI
 * polarity, byte bit order, the frame check sequence, and continuous-phase
 * tone generation are each individually easy to get backwards and
 * individually invisible until something fails to decode; a round trip pins
 * all of them at once, with no audio device, no timing, and no radio.
 */

#include <catch2/catch_test_macros.hpp>

#include "protocols/APRS/Demodulator.hpp"
#include "protocols/APRS/Modulator.hpp"
#include "protocols/APRS/packet.h"

#include <cstring>
#include <vector>

namespace
{

/** A frame the way the decoder hands one back: raw bytes and a length. */
struct frameData {
    uint8_t data[APRS_PACLEN];
    uint8_t len;
};

/**
 * Modulator that keeps its baseband instead of playing it.
 */
class CapturingModulator : public APRS::Modulator
{
public:
    std::vector<int16_t> samples;

protected:
    void emitBlock(const stream_sample_t *block, size_t len) override
    {
        samples.insert(samples.end(), block, block + len);
    }
};

/**
 * Decimation factor from the transmit rate to the rate the demodulator runs
 * at.  Both are fixed and the ratio is a whole number, so the conversion is
 * a plain sample drop: the Bell 202 tones top out at 2200 Hz, comfortably
 * below the 4800 Hz Nyquist limit that survives, so nothing aliases into the
 * passband and no anti-alias filter is needed.
 */
constexpr size_t DECIMATION = APRS::Modulator::TX_SAMPLE_RATE
                            / APRS_SAMPLE_RATE;

static_assert(APRS::Modulator::TX_SAMPLE_RATE % APRS_SAMPLE_RATE == 0,
              "transmit rate must be a whole multiple of the receive rate");

/**
 * Peak level the demodulator is fed at.
 *
 * The transmit chain generates at Modulator::TX_AMPLITUDE, which is sized to
 * drive a radio's modulator input, not a demodulator directly.  The
 * demodulator's mark and space correlators run in int16 arithmetic and
 * saturate above roughly 500 peak, so a loopback at transmit level would
 * fail to decode for reasons that have nothing to do with the modulator.
 * That headroom limit is a real robustness gap on the receive side — a
 * strong signal on a real radio hits it too — and is tracked separately; it
 * is not something this test can or should paper over silently, hence this
 * explicit, documented rescale.
 */
constexpr int32_t RX_PEAK = 300;

/**
 * Resample and rescale captured transmit baseband into something the
 * demodulator can be fed.
 */
std::vector<int16_t> toReceiveBaseband(const std::vector<int16_t> &tx)
{
    int32_t peak = 1;
    for (int16_t s : tx) {
        const int32_t mag = (s < 0) ? -(int32_t)s : (int32_t)s;
        if (mag > peak)
            peak = mag;
    }

    std::vector<int16_t> rx;
    rx.reserve(tx.size() / DECIMATION);
    for (size_t i = 0; i < tx.size(); i += DECIMATION)
        rx.push_back((int16_t)(((int32_t)tx[i] * RX_PEAK) / peak));

    return rx;
}

/**
 * Push baseband through the demodulator in stream-sized blocks, returning
 * every frame it completes.
 */
std::vector<frameData> demodulate(std::vector<int16_t> &rx)
{
    APRS::Demodulator demod;
    demod.init();

    std::vector<frameData> frames;

    for (size_t i = 0; i < rx.size(); i += APRS_BUF_SIZE) {
        dataBlock_t block;
        block.data = &rx[i];
        block.len = ((rx.size() - i) < APRS_BUF_SIZE) ? (rx.size() - i) :
                                                        APRS_BUF_SIZE;

        if (demod.update(block)) {
            frameData f;
            ssize_t n = demod.getFrame(f.data, sizeof(f.data));
            if (n > 0) {
                f.len = (uint8_t)n;
                frames.push_back(f);
            }
        }
    }

    return frames;
}

/**
 * Modulate one frame with a realistic preamble and tail, then demodulate.
 */
std::vector<frameData> loopback(const frameData &frame)
{
    CapturingModulator mod;

    mod.init();
    REQUIRE(mod.start() == true);
    /* A short key-up sequence: the demodulator needs a few symbol times of
     * tone to settle before the frame starts. */
    mod.sendFlags(100);
    mod.sendFrame(frame.data, frame.len);
    mod.sendFlags(20);
    mod.stop();
    mod.terminate();

    REQUIRE(mod.samples.empty() == false);

    std::vector<int16_t> rx = toReceiveBaseband(mod.samples);
    return demodulate(rx);
}

} // namespace

TEST_CASE("APRS modulator: a modulated frame demodulates back", "[aprs]")
{
    frameData tx;
    const char info[] = ":W1AW     :hello there";

    REQUIRE((tx.len = (uint8_t)aprsFrameBuild(
                 tx.data, sizeof(tx.data), APRS_TOCALL, "N0CALL-7",
                 APRS_DEFAULT_PATH, info, strlen(info)))
            > 0);

    std::vector<frameData> frames = loopback(tx);

    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].len == tx.len);
    REQUIRE(memcmp(frames[0].data, tx.data, tx.len) == 0);
}

TEST_CASE("APRS modulator: the round trip preserves the whole packet", "[aprs]")
{
    frameData tx;
    char info[APRS_MSG_ADDRESSEE_LEN + APRS_MSG_TEXT_MAX + 3];

    REQUIRE(aprsMsgFormat(info, sizeof(info), "W1AW-9", "meet on 146.52") > 0);
    REQUIRE((tx.len = (uint8_t)aprsFrameBuild(
                 tx.data, sizeof(tx.data), APRS_TOCALL, "N0CALL-7",
                 APRS_DEFAULT_PATH, info, strlen(info)))
            > 0);

    std::vector<frameData> frames = loopback(tx);
    REQUIRE(frames.size() == 1);

    aprsPacket pkt;
    REQUIRE(aprsPktFromFrame(frames[0].data, frames[0].len, &pkt) == true);

    char addr[APRS_ADDR_STR_LEN];
    aprsAddrToStr(&pkt.addresses[0], addr, sizeof(addr));
    REQUIRE(strcmp(addr, APRS_TOCALL) == 0);
    aprsAddrToStr(&pkt.addresses[1], addr, sizeof(addr));
    REQUIRE(strcmp(addr, "N0CALL-7") == 0);

    char path[32];
    aprsPathToStr(&pkt, path, sizeof(path));
    REQUIRE(strcmp(path, APRS_DEFAULT_PATH) == 0);

    char addressee[APRS_ADDR_STR_LEN];
    char text[APRS_PACLEN];
    REQUIRE(pkt.type == APRS_TYPE_MESSAGE);
    REQUIRE(
        aprsMsgUnwrap(&pkt, addressee, sizeof(addressee), text, sizeof(text))
        == true);
    REQUIRE(strcmp(addressee, "W1AW-9") == 0);
    REQUIRE(strcmp(text, "meet on 146.52") == 0);
}

TEST_CASE("APRS modulator: bit stuffing survives data that looks like a flag",
          "[aprs]")
{
    /* 0x7e is the flag byte and 0xff is six ones and then some: without bit
     * stuffing either one inside the info field would terminate the frame
     * early or be rejected outright.  '~' is 0x7e in ASCII, so this is a
     * message a user could plausibly type. */
    frameData tx;
    const char info[] = ":W1AW     :~~~~~~~~ and back";

    REQUIRE((tx.len = (uint8_t)aprsFrameBuild(tx.data, sizeof(tx.data),
                                              APRS_TOCALL, "N0CALL-7", nullptr,
                                              info, strlen(info)))
            > 0);

    std::vector<frameData> frames = loopback(tx);

    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].len == tx.len);
    REQUIRE(memcmp(frames[0].data, tx.data, tx.len) == 0);
}

TEST_CASE("APRS modulator: back-to-back frames both decode", "[aprs]")
{
    frameData first;
    frameData second;

    const char infoA[] = ":W1AW     :first";
    const char infoB[] = ":W1AW     :second";

    REQUIRE((first.len = (uint8_t)aprsFrameBuild(
                 first.data, sizeof(first.data), APRS_TOCALL, "N0CALL-7",
                 APRS_DEFAULT_PATH, infoA, strlen(infoA)))
            > 0);
    REQUIRE((second.len = (uint8_t)aprsFrameBuild(
                 second.data, sizeof(second.data), APRS_TOCALL, "N0CALL-7",
                 APRS_DEFAULT_PATH, infoB, strlen(infoB)))
            > 0);

    CapturingModulator mod;
    mod.init();
    REQUIRE(mod.start() == true);
    mod.sendFlags(100);
    mod.sendFrame(first.data, first.len);
    mod.sendFlags(20);
    mod.sendFrame(second.data, second.len);
    mod.sendFlags(20);
    mod.stop();
    mod.terminate();

    std::vector<int16_t> rx = toReceiveBaseband(mod.samples);
    std::vector<frameData> frames = demodulate(rx);

    REQUIRE(frames.size() == 2);
    REQUIRE(frames[0].len == first.len);
    REQUIRE(memcmp(frames[0].data, first.data, first.len) == 0);
    REQUIRE(frames[1].len == second.len);
    REQUIRE(memcmp(frames[1].data, second.data, second.len) == 0);
}

TEST_CASE("APRS modulator: the flag sequence is the length that was asked for",
          "[aprs]")
{
    CapturingModulator mod;

    mod.init();
    REQUIRE(mod.start() == true);
    mod.sendFlags(300);
    mod.stop();

    /* 300 ms of flags is 45 whole flags — 360 symbols — and stop() rounds up
     * to the end of the block in progress. */
    const size_t symbols = 45 * 8;
    const size_t expected = symbols * APRS::Modulator::SAMPLES_PER_SYMBOL;
    REQUIRE(mod.samples.size() >= expected);
    REQUIRE(mod.samples.size() < (expected + APRS::Modulator::BLOCK_SAMPLES));

    mod.terminate();
}

TEST_CASE("APRS modulator: transmit level stays inside the sample range",
          "[aprs]")
{
    frameData tx;
    const char info[] = ":W1AW     :level check";

    REQUIRE((tx.len = (uint8_t)aprsFrameBuild(
                 tx.data, sizeof(tx.data), APRS_TOCALL, "N0CALL-7",
                 APRS_DEFAULT_PATH, info, strlen(info)))
            > 0);

    CapturingModulator mod;
    mod.init();
    REQUIRE(mod.start() == true);
    mod.sendFlags(50);
    mod.sendFrame(tx.data, tx.len);
    mod.stop();
    mod.terminate();

    int32_t peak = 0;
    for (int16_t s : mod.samples) {
        const int32_t mag = (s < 0) ? -(int32_t)s : (int32_t)s;
        if (mag > peak)
            peak = mag;
    }

    /* The tone must reach the level it was configured for — a modulator that
     * silently under-drives produces a signal no receiver can decode — and
     * must not exceed it, because clipping here becomes splatter on the
     * air. */
    REQUIRE(peak <= APRS::Modulator::TX_AMPLITUDE);
    REQUIRE(peak > ((APRS::Modulator::TX_AMPLITUDE * 9) / 10));
}
