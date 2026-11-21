/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <pthread.h>
#include <thread>

#include "core/cat/CatAnswer.hpp"
#include "core/cat/CatParser.hpp"
#include "core/cat/cat.h"
#include "core/cps.h"
#include "core/state.h"
#include "interfaces/delays.h"
#include "rtx/rtx.h"

/**
 * Text collected from a parser, an answer or the CAT link, null-terminated.
 */
using Text = std::array<char, 256>;

static Text hostToRadio;
static size_t hostToRadioLen;
static size_t hostToRadioPos;
static Text radioToHost;
static size_t radioToHostLen;

/**
 * Loopback device: the radio reads what the host sent.
 */
static ssize_t loopRead(void *buf, size_t len)
{
    size_t count = std::min(len, hostToRadioLen - hostToRadioPos);

    memcpy(buf, &hostToRadio[hostToRadioPos], count);
    hostToRadioPos += count;
    return count;
}

/**
 * Loopback device: the radio's answers go to the host.
 */
static ssize_t loopWrite(const void *buf, size_t len)
{
    memcpy(&radioToHost[radioToHostLen], buf, len);
    radioToHostLen += len;
    return len;
}

static const struct chardev_ops loopDev = {
    .init = nullptr,
    .read = loopRead,
    .write = loopWrite,
};

/**
 * Send commands to the radio and collect its answers.
 *
 * @param cmds: commands as the host writes them.
 * @param now: current time in milliseconds.
 * @return answers.
 */
static Text cat(const char *cmds, const long long now = 1)
{
    hostToRadioLen = strlen(cmds);
    hostToRadioPos = 0;
    memcpy(hostToRadio.data(), cmds, hostToRadioLen);
    radioToHost = {};
    radioToHostLen = 0;

    while (hostToRadioPos < hostToRadioLen)
        cat_task(now);

    return radioToHost;
}

/**
 * Check the answers to commands.
 *
 * @param cmds: commands as the host writes them.
 * @param expected: answers the radio must send.
 * @return true if the radio sent exactly the expected answers.
 */
static bool answers(const char *cmds, const char *expected)
{
    return strcmp(cat(cmds).data(), expected) == 0;
}

/**
 * Feed text to a parser and collect the commands it completes, each followed
 * by '|'.
 *
 * @param parser: parser under test.
 * @param text: bytes received from the link.
 * @return completed commands.
 */
static Text parse(CatParser &parser, const char *text)
{
    Text out = {};
    size_t len = 0;

    for (size_t i = 0; text[i] != '\0'; i++) {
        if (parser.push(text[i]) == false)
            continue;

        memcpy(&out[len], parser.data(), parser.size());
        len += parser.size();
        out[len++] = '|';
    }

    return out;
}

/**
 * Get the text of an answer.
 *
 * @param answer: answer.
 * @return answer text.
 */
static Text textOf(const CatAnswer &answer)
{
    Text out = {};

    memcpy(out.data(), answer.data(), answer.size());
    return out;
}

TEST_CASE("CAT parser splits commands on ';'", "[cat][parser]")
{
    CatParser parser;

    SECTION("Commands, empty commands and line ends")
    {
        REQUIRE(strcmp(parse(parser, ";;FA;\r\nMD0;ID").data(), "FA|MD0|")
                == 0);

        // The unfinished command carries on with the next bytes
        REQUIRE(strcmp(parse(parser, ";").data(), "ID|") == 0);
    }

    SECTION("A control byte discards the command in progress")
    {
        REQUIRE(strcmp(parse(parser, "I\001D;").data(), "D|") == 0);
    }

    SECTION("An overlong command completes empty")
    {
        char text[64];
        memset(text, 'A', 40);
        strcpy(&text[40], ";ID;");

        REQUIRE(strcmp(parse(parser, text).data(), "|ID|") == 0);
    }
}

TEST_CASE("CAT answer builds fixed-width fields", "[cat][answer]")
{
    CatAnswer answer;

    SECTION("Text and zero-padded numbers")
    {
        answer.append("FA", 2);
        answer.appendNumber(145500000, 9);
        answer.append('X');
        answer.appendNumber(7, 3);
        answer.finish();

        REQUIRE(strcmp(textOf(answer).data(), "FA145500000X007;") == 0);
    }

    SECTION("Digits beyond the width and text past the length are dropped")
    {
        answer.appendNumber(1234, 2);
        answer.append("ABCDEF", 3);
        answer.finish();

        REQUIRE(strcmp(textOf(answer).data(), "34ABC;") == 0);
    }

    SECTION("Hexadecimal numbers")
    {
        answer.appendNumber(0x3F, 4, 16);
        answer.appendNumber(0xABC, 2, 16);
        answer.finish();

        REQUIRE(strcmp(textOf(answer).data(), "003FBC;") == 0);
    }

    SECTION("A full answer still ends with ';'")
    {
        for (int i = 0; i < 100; i++)
            answer.append('A');
        answer.finish();

        REQUIRE(answer.size() == 49);
        REQUIRE(answer.data()[48] == ';');
    }
}

/**
 * Start CAT on the loopback, with a 145.500 MHz FM simplex channel.
 */
static void setUp()
{
    REQUIRE(cat_init(&loopDev) == 0);

    state.txDisable = false;
    state.channel = channel_t();
    state.channel.mode = OPMODE_FM;
    state.channel.bandwidth = BW_25;
    state.channel.power = 1000;
    state.channel.rx_frequency = 145500000;
    state.channel.tx_frequency = 145500000;
}

TEST_CASE("CAT reads, silent sets and errors", "[cat][commands]")
{
    setUp();

    REQUIRE(answers("ID;", "ID0999;"));
    REQUIRE(answers("AI;AI0;", "AI0;"));
    REQUIRE(answers("AI1;", "?;"));
    REQUIRE(answers("PS;PS1;", "PS1;"));
    REQUIRE(answers("PS0;", "?;"));
    REQUIRE(answers("FA;", "FA145500000;"));
    REQUIRE(answers("IF;", "IF000145500000+000000400000;"));
    REQUIRE(answers("XX;", "?;"));
    REQUIRE(answers("F;", "?;"));
    REQUIRE(answers("FA1;", "?;"));
    REQUIRE(answers("ID1;", "?;"));
    REQUIRE(answers("ZV;", "ZV" GIT_VERSION ";"));
    REQUIRE(answers("ZR;", "ZR000;"));
}

TEST_CASE("CAT frequency, repeater shift and offset", "[cat][commands]")
{
    setUp();

    SECTION("FA keeps the shift, OS and EX set it")
    {
        REQUIRE(answers("FA146940000;", ""));
        REQUIRE(state.channel.tx_frequency == 146940000);
        REQUIRE(answers("OS02;OS0;", "OS02;"));
        REQUIRE(state.channel.tx_frequency == 146340000);
        REQUIRE(answers("EX082;", "EX0820600;"));

        // The offset follows the frequency
        REQUIRE(answers("FA146760000;", ""));
        REQUIRE(state.channel.tx_frequency == 146160000);

        // Hamlib sends four digits of kHz for every band
        REQUIRE(answers("EX0820500;", ""));
        REQUIRE(state.channel.tx_frequency == 146260000);
        REQUIRE(answers("ZO;", "ZO000500000;"));
        REQUIRE(answers("ZO1234567;OS01;", ""));
        REQUIRE(state.channel.tx_frequency == 146760000 + 1234567);

        // One offset for every band, five digits in the 430 MHz menu
        REQUIRE(answers("FA446000000;", ""));
        REQUIRE(state.channel.tx_frequency == 446000000 + 1234567);
        REQUIRE(answers("EX083;EX080;", "EX08301234;EX0801234;"));

        // A simplex channel keeps the offset for the next OS
        REQUIRE(answers("OS00;ZO;", "ZO001234567;"));
        REQUIRE(answers("EX0805000;OS02;", ""));
        REQUIRE(state.channel.tx_frequency == 441000000);

        REQUIRE(answers("EX0820600;", ""));
    }

    SECTION("FB sets the TX frequency, out-of-band frequencies are refused")
    {
        REQUIRE(answers("FB146000000;FB;", "FB146000000;"));
        REQUIRE(answers("OS0;", "OS01;"));
        REQUIRE(answers("FA050000000;", "?;"));
        REQUIRE(answers("FB050000000;", "?;"));
        REQUIRE(state.channel.tx_frequency == 146000000);

        // A shift that would leave the band
        REQUIRE(answers("FA136100000;OS00;OS02;", "?;"));
        REQUIRE(state.channel.tx_frequency == 136100000);
        REQUIRE(answers("OS03;EX084;", "?;?;"));
    }

    SECTION("CAT tunes the VFO")
    {
        // A frequency change on a memory channel goes to VFO mode
        state.tuner_mode = CH;
        REQUIRE(answers("FA146520000;", ""));
        REQUIRE(state.tuner_mode == VFO);
        REQUIRE(state.vfo_channel.rx_frequency == 146520000);

        // In VFO mode the VFO copy follows other changes too
        REQUIRE(answers("PC002;", ""));
        REQUIRE(state.vfo_channel.power == 2000);

        // On a memory channel other changes stay on the channel
        state.tuner_mode = CH;
        REQUIRE(answers("PC003;", ""));
        REQUIRE(state.tuner_mode == CH);
        REQUIRE(state.vfo_channel.power == 2000);

        // An offset on a simplex channel only sets what OS applies next
        REQUIRE(answers("ZO000700000;ZO;", "ZO000700000;"));
        REQUIRE(state.tuner_mode == CH);
        REQUIRE(answers("ZO000600000;", ""));
        state.tuner_mode = VFO;
    }

    SECTION("What Hamlib's FT-991 backend sends around a frequency set")
    {
        REQUIRE(answers("EX0321;EX032;", "EX0321;"));
        REQUIRE(answers("BS15;FA146520000;FT2;FT3;FT;", "FT0;"));
        REQUIRE(state.channel.rx_frequency == 146520000);
        REQUIRE(answers("BS;", "?;"));
    }
}

TEST_CASE("CAT mode and tones", "[cat][commands]")
{
    setUp();

    SECTION("MD, NA and SH")
    {
        REQUIRE(answers("MD0;", "MD04;"));
        REQUIRE(answers("MD0B;NA0;", "NA01;"));
        REQUIRE(state.channel.bandwidth == BW_12_5);
        REQUIRE(answers("MD04;NA00;SH0;", "SH000;"));
        REQUIRE(state.channel.bandwidth == BW_25);
        REQUIRE(answers("MD02;", "?;"));
    }

    SECTION("CN and CT, with OpenRTX's own tone index")
    {
        REQUIRE(answers("CN00012;CT01;CN00;CT0;", "CN00012;CT01;"));
        REQUIRE(state.channel.fm.txTone == CTCSS_100_0);
        REQUIRE(state.channel.fm.rxToneEn == 1);

        REQUIRE(answers("CN00049;CT02;", ""));
        REQUIRE(state.channel.fm.txTone == CTCSS_254_1);
        REQUIRE(state.channel.fm.rxToneEn == 0);
        REQUIRE(answers("IF;", "IF000145500000+000000402000;"));
        REQUIRE(answers("CN00050;CN01;CT03;", "?;?;?;"));
    }

    SECTION("M17 lives only in the OpenRTX commands")
    {
        REQUIRE(answers("ZM;ZM02;ZM;", "ZM00;ZM02;"));
        REQUIRE(state.channel.mode == OPMODE_M17);
        REQUIRE(answers("MD0;NA0;IF;CN00;CT0;", "?;?;?;?;?;"));
        REQUIRE(answers("FA;", "FA145500000;"));

        // A Yaesu mode switches back to FM
        REQUIRE(answers("MD0B;ZM;", "ZM01;"));
        REQUIRE(state.channel.mode == OPMODE_FM);
    }
}

TEST_CASE("CAT power and levels", "[cat][commands]")
{
    setUp();

    REQUIRE(answers("PC002;PC;", "PC002;"));
    REQUIRE(state.channel.power == 2000);
    REQUIRE(answers("PC000;PC101;", "?;?;"));
    REQUIRE(answers("ZP000500;ZP;PC;", "ZP000500;PC001;"));
    REQUIRE(answers("ZP0;", "?;"));

    REQUIRE(answers("SQ0060;SQ0;", "SQ0060;"));
    REQUIRE(state.settings.sqlLevel == 9);
    REQUIRE(answers("SQ0101;", "?;"));

    state.rssi = -73;
    REQUIRE(answers("SM0;", "SM0130;"));
    state.rssi = -127;
    REQUIRE(answers("SM0;", "SM0000;"));
    state.rssi = -20;
    REQUIRE(answers("SM0;", "SM0172;"));
}

TEST_CASE("CAT M17 settings", "[cat][commands]")
{
    setUp();

    REQUIRE(answers("ZCab1cd;ZC;", "ZCAB1CD;"));
    REQUIRE(strcmp(state.settings.callsign, "AB1CD") == 0);
    REQUIRE(answers("ZCa*b;ZC0123456789;", "?;?;"));

    REQUIRE(answers("ZDm17-m17 c;ZD;", "ZDM17-M17 C;"));
    REQUIRE(answers("ZDALL;ZD;", "ZDALL;"));
    REQUIRE(state.settings.m17_dest[0] == '\0');

    REQUIRE(answers("ZN05;ZN;", "ZN05;"));
    REQUIRE(state.settings.m17_can == 5);
    REQUIRE(answers("ZN16;", "?;"));
}

TEST_CASE("CAT holds the PTT for at most two minutes", "[cat][commands]")
{
    static constexpr long long TX_TIMEOUT_MS = 120000;

    setUp();

    // Keyed long enough ago that 1 s of the limit is left
    long long keyed = getTick() - TX_TIMEOUT_MS + 1000;
    REQUIRE(answers("TX;", "TX0;"));
    REQUIRE(strcmp(cat("TX1;TX;", keyed).data(), "TX1;") == 0);
    REQUIRE(rtx_pttActive());

    // Keying again does not extend the limit
    REQUIRE(strcmp(cat("TX1;", getTick()).data(), "") == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    REQUIRE_FALSE(rtx_pttActive());
    REQUIRE(answers("TX;", "TX0;"));

    REQUIRE(strcmp(cat("TX1;", getTick()).data(), "") == 0);
    REQUIRE(rtx_pttActive());
    REQUIRE(answers("TX0;", ""));
    REQUIRE_FALSE(rtx_pttActive());
    REQUIRE(answers("TX2;", "?;"));

    // Refused where the radio's own PTT is (a menu, an RX-only channel), as
    // the rtx module has it configured
    static pthread_mutex_t rtxMutex = PTHREAD_MUTEX_INITIALIZER;
    rtx_init(&rtxMutex);
    rtxStatus_t cfg = rtx_getCurrentStatus();
    cfg.txDisable = 1;
    rtx_configure(&cfg);
    rtx_task();

    REQUIRE(answers("TX1;", "?;"));
    REQUIRE_FALSE(rtx_pttActive());

    cfg.txDisable = 0;
    rtx_configure(&cfg);
    rtx_task();
    rtx_terminate();
}

TEST_CASE("CAT refuses the bootloader without CONFIG_BOOTLOADER_ENTRY",
          "[cat][commands]")
{
    setUp();

    REQUIRE(answers("ZB;ZB0;ZB1;", "?;?;?;"));
}
