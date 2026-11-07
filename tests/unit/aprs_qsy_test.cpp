/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include "core/aprs_qsy.h"
#include "rtx/rtx.h"
#include <cstring>
#include <string>

namespace
{

aprsFreq announced(uint32_t freq)
{
    aprsFreq f;
    memset(&f, 0, sizeof(f));
    f.freq = freq;
    return f;
}

int32_t shiftOf(uint32_t freq)
{
    aprsFreq f = announced(freq);
    return aprsQsy_offset(&f);
}

/* A VFO with settings tuning must keep */
channel_t vfo()
{
    channel_t ch;
    memset(&ch, 0, sizeof(ch));
    ch.mode = OPMODE_APRS;
    ch.rx_frequency = 144390000;
    ch.tx_frequency = 144390000;
    ch.power = 5000;
    ch.bandwidth = BW_12_5;
    strcpy(ch.name, "VFO");
    return ch;
}

} // namespace

TEST_CASE("APRS QSY: the band's standard shift when none is sent",
          "[aprs][qsy]")
{
    CHECK(shiftOf(146820000) == -600000);
    CHECK(shiftOf(146880000) == -600000);
    CHECK(shiftOf(145450000) == -600000);
    CHECK(shiftOf(146340000) == 600000);
    CHECK(shiftOf(147360000) == 600000);
    CHECK(shiftOf(147915000) == -600000);
    CHECK(shiftOf(224420000) == -1600000);
    CHECK(shiftOf(443200000) == 5000000);
    CHECK(shiftOf(449500000) == -5000000);

    /* Simplex channels, hotspots and data */
    CHECK(shiftOf(146520000) == 0);
    CHECK(shiftOf(147450000) == 0);
    CHECK(shiftOf(441112500) == 0);
    CHECK(shiftOf(144990000) == 0);

    /* What was sent wins, simplex included */
    aprsFreq f = announced(146820000);
    f.flags = APRS_FREQ_HAS_OFFSET;
    f.offset = 0;
    CHECK(aprsQsy_offset(&f) == 0);
    f.offset = 600000;
    CHECK(aprsQsy_offset(&f) == 600000);
}

TEST_CASE("APRS QSY: tones sent without their tenths", "[aprs][qsy]")
{
    CHECK(ctcss_tone[aprsQsy_toneIndex(107)] == 1072);
    CHECK(ctcss_tone[aprsQsy_toneIndex(100)] == 1000);
    CHECK(ctcss_tone[aprsQsy_toneIndex(67)] == 670);
    CHECK(ctcss_tone[aprsQsy_toneIndex(254)] == 2541);
    CHECK(aprsQsy_toneIndex(108) == CTCSS_FREQ_NUM);
    CHECK(aprsQsy_toneIndex(0) == CTCSS_FREQ_NUM);
}

TEST_CASE("APRS QSY: an FM repeater", "[aprs][qsy]")
{
    aprsFreq f = announced(146820000);
    f.toneType = APRS_TONE_ENCODE;
    f.tone = 107;

    channel_t ch = vfo();
    REQUIRE(aprsQsy_tune(&f, &ch) == APRS_QSY_OK);
    CHECK(ch.mode == OPMODE_FM);
    CHECK(ch.rx_frequency == 146820000);
    CHECK(ch.tx_frequency == 146220000);
    CHECK(ch.fm.txToneEn == 1);
    CHECK(ctcss_tone[ch.fm.txTone] == 1072);
    CHECK(ch.fm.rxToneEn == 0);
    CHECK(ch.bandwidth == BW_25);

    /* The rest of the VFO stays */
    CHECK(ch.power == 5000);
    CHECK(std::string(ch.name) == "VFO");

    /* "C107": tone squelch too; lower case: narrow */
    f.toneType = APRS_TONE_SQUELCH;
    f.flags = APRS_FREQ_NARROW;
    ch = vfo();
    REQUIRE(aprsQsy_tune(&f, &ch) == APRS_QSY_OK);
    CHECK(ch.fm.txToneEn == 1);
    CHECK(ch.fm.rxToneEn == 1);
    CHECK(ctcss_tone[ch.fm.rxTone] == 1072);
    CHECK(ch.bandwidth == BW_12_5);

    /* No tone, sent offset */
    f = announced(443200000);
    f.flags = APRS_FREQ_HAS_OFFSET;
    f.offset = 5000000;
    ch = vfo();
    ch.fm.txToneEn = 1;
    ch.fm.rxToneEn = 1;
    REQUIRE(aprsQsy_tune(&f, &ch) == APRS_QSY_OK);
    CHECK(ch.tx_frequency == 448200000);
    CHECK(ch.fm.txToneEn == 0);
    CHECK(ch.fm.rxToneEn == 0);
}

TEST_CASE("APRS QSY: an M17 hotspot", "[aprs][qsy]")
{
    aprsFreq f = announced(441112500);
    f.mode = APRS_FREQ_M17;
    f.can = 3;
    f.flags = APRS_FREQ_HAS_CAN;

    channel_t ch = vfo();
    REQUIRE(aprsQsy_tune(&f, &ch) == APRS_QSY_OK);
    CHECK(ch.mode == OPMODE_M17);
    CHECK(ch.rx_frequency == 441112500);
    CHECK(ch.tx_frequency == 441112500);
    CHECK(ch.m17.rxCan == 3);
    CHECK(ch.m17.txCan == 3);

    /* No CAN sent: 0 */
    f.flags = 0;
    ch = vfo();
    ch.m17.rxCan = 9;
    ch.m17.txCan = 9;
    REQUIRE(aprsQsy_tune(&f, &ch) == APRS_QSY_OK);
    CHECK(ch.m17.rxCan == 0);
    CHECK(ch.m17.txCan == 0);

    /* A tone this radio cannot make does not matter in M17 */
    f.toneType = APRS_TONE_DCS;
    CHECK(aprsQsy_tune(&f, nullptr) == APRS_QSY_OK);
}

TEST_CASE("APRS QSY: what this radio cannot work", "[aprs][qsy]")
{
    const uint8_t modes[] = { APRS_FREQ_DMR, APRS_FREQ_DSTAR, APRS_FREQ_FUSION,
                              APRS_FREQ_P25, APRS_FREQ_NXDN,  APRS_FREQ_DATA };
    for (uint8_t m : modes) {
        INFO("mode " << (int)m);
        aprsFreq f = announced(444175000);
        f.mode = m;
        channel_t ch = vfo();
        CHECK(aprsQsy_tune(&f, &ch) == APRS_QSY_MODE);
        CHECK(ch.mode == OPMODE_APRS);
        CHECK(ch.rx_frequency == 144390000);
    }

    aprsFreq f = announced(146805000);
    f.toneType = APRS_TONE_DCS;
    f.tone = 256;
    CHECK(aprsQsy_tune(&f, nullptr) == APRS_QSY_TONE);

    f.toneType = APRS_TONE_BURST;
    f.tone = 1750;
    CHECK(aprsQsy_tune(&f, nullptr) == APRS_QSY_TONE);

    f.toneType = APRS_TONE_ENCODE;
    f.tone = 108;
    CHECK(aprsQsy_tune(&f, nullptr) == APRS_QSY_TONE);

    /* 1.25 m is outside the emulator's bands */
    f = announced(224420000);
    CHECK(aprsQsy_tune(&f, nullptr) == APRS_QSY_BAND);

    /* So is a transmit frequency the offset pushes out */
    f = announced(479000000);
    f.flags = APRS_FREQ_HAS_OFFSET;
    f.offset = 5000000;
    channel_t ch = vfo();
    CHECK(aprsQsy_tune(&f, &ch) == APRS_QSY_BAND);
    CHECK(ch.rx_frequency == 144390000);

    f.offset = -5000000;
    CHECK(aprsQsy_tune(&f, nullptr) == APRS_QSY_OK);
}
