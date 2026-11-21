/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Tests of the CAT debug commands (CONFIG_CAT_DEBUG): dispatch to the
 * registered commands, their parameters, calls on the RTX thread and the
 * debug lines the radio sends on its own.
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <thread>

#include "core/cat/cat.h"
#include "core/cat/debug.hpp"

/**
 * Loopback of the link: what the host sends and what the radio answers.
 */
using Text = std::array<char, 8192>;

static Text hostToRadio;
static size_t hostToRadioLen;
static size_t hostToRadioPos;
static Text radioToHost;
static size_t radioToHostLen;

/**
 * Read of the loopback device: the bytes the host sent.
 *
 * @param buf: destination buffer.
 * @param len: size of buf.
 * @return number of bytes read.
 */
static ssize_t loopRead(void *buf, size_t len)
{
    size_t count = std::min(len, hostToRadioLen - hostToRadioPos);

    memcpy(buf, &hostToRadio[hostToRadioPos], count);
    hostToRadioPos += count;
    return count;
}

/**
 * Write of the loopback device: collect what the radio sends.
 *
 * @param buf: bytes to send.
 * @param len: number of bytes.
 * @return number of bytes written.
 */
static ssize_t loopWrite(const void *buf, size_t len)
{
    size_t count = std::min(len, radioToHost.size() - 1 - radioToHostLen);

    memcpy(&radioToHost[radioToHostLen], buf, count);
    radioToHostLen += count;
    return count;
}

static const struct chardev_ops loopDev = {
    .init = nullptr,
    .read = loopRead,
    .write = loopWrite,
};

/**
 * Send commands to the radio and collect what it sends back.
 *
 * @param cmds: commands as the host writes them.
 * @return what the radio sent.
 */
static Text cat(const char *cmds)
{
    hostToRadioLen = strlen(cmds);
    hostToRadioPos = 0;
    memcpy(hostToRadio.data(), cmds, hostToRadioLen);
    radioToHost = {};
    radioToHostLen = 0;

    do {
        cat_task(1);
    } while (hostToRadioPos < hostToRadioLen);

    return radioToHost;
}

/**
 * Check what the radio sends back to commands.
 *
 * @param cmds: commands as the host writes them.
 * @param expected: what the radio must send.
 * @return true if the radio sent exactly that.
 */
static bool answers(const char *cmds, const char *expected)
{
    return strcmp(cat(cmds).data(), expected) == 0;
}

/**
 * Registers of a pretend chip, which only the RTX thread may touch.
 */
static std::array<uint8_t, 256> chipRegs;
static std::atomic<bool> onRtxThread;

/**
 * ZZT1: read (ZZT1<reg>;) or write (ZZT1<reg> <value>;) a register of the
 * pretend chip, on the RTX thread.
 */
static int testCommand(const CatRequest &req, CatAnswer &answer)
{
    std::array<uint32_t, 2> arg;
    int count = cat_debugParseHex(req, arg.data(), arg.size());
    if ((count < 1) || (arg[0] > 0xFF))
        return -EINVAL;

    uint8_t value = 0;
    int ret = cat_debugOnRtx([count, &arg, &value]() {
        if (onRtxThread == false)
            return -EFAULT;

        if (count == 2)
            chipRegs[arg[0]] = arg[1];
        else
            value = chipRegs[arg[0]];

        return 0;
    });

    if ((ret < 0) || (count == 2))
        return ret;

    answer.append("ZZT1", 4);
    answer.appendNumber(arg[0], 2, 16);
    answer.append(' ');
    answer.appendNumber(value, 2, 16);
    return 0;
}

static CatDebugCommand testCmd = { { 'T', '1' }, testCommand, nullptr };
static CatDebugCommand otherCmd = { { 'T', '2' }, testCommand, nullptr };
static CatDebugCommand clashCmd = { { 'T', '1' }, testCommand, nullptr };

/**
 * Run the RTX thread's side of the debug calls while in scope.
 */
class RtxThread
{
public:
    RtxThread() : running(true), thread([this]() { loop(); })
    {
    }

    ~RtxThread()
    {
        running = false;
        thread.join();
    }

private:
    void loop()
    {
        onRtxThread = true;
        while (running) {
            cat_debugService();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        onRtxThread = false;
    }

    std::atomic<bool> running;
    std::thread thread;
};

TEST_CASE("CAT debug commands are listed and dispatched", "[cat][debug]")
{
    REQUIRE(cat_init(&loopDev) == 0);

    cat_debugRegister(testCmd);
    cat_debugRegister(otherCmd);
    cat_debugRegister(testCmd);
    cat_debugRegister(clashCmd);

    SECTION("Registered once each, a taken name ignored")
    {
        // Test cases run in any order, and so register in any order
        Text list = cat("ZZ;");
        bool known = (strcmp(list.data(), "ZZT2T1;") == 0)
                  || (strcmp(list.data(), "ZZT1T2;") == 0);
        REQUIRE(known);
    }

    SECTION("Unknown or incomplete commands answer an error")
    {
        REQUIRE(answers("ZZT9;", "?;"));
        REQUIRE(answers("ZZT;", "?;"));
        REQUIRE(answers("ZZT1;", "?;"));
    }
}

TEST_CASE("CAT debug parameters are hexadecimal numbers", "[cat][debug]")
{
    std::array<uint32_t, 2> arg = {};

    auto parse = [&arg](const char *text) {
        CatRequest req = { text, strlen(text), 0 };
        return cat_debugParseHex(req, arg.data(), arg.size());
    };

    REQUIRE(parse("") == 0);
    REQUIRE(parse(" 1f  A0 ") == 2);
    REQUIRE(arg[0] == 0x1F);
    REQUIRE(arg[1] == 0xA0);
    REQUIRE(parse("10") == 1);

    REQUIRE(parse("1 2 3") == -EINVAL);
    REQUIRE(parse("-1") == -EINVAL);
    REQUIRE(parse("1G") == -EINVAL);
    REQUIRE(parse("100000000") == -EINVAL);
    REQUIRE(parse("FFFFFFFF") == 1);
    REQUIRE(arg[0] == 0xFFFFFFFF);
    REQUIRE(parse("012345678901234567890123456789012") == -EINVAL);
}

TEST_CASE("CAT debug commands run on the RTX thread", "[cat][debug]")
{
    REQUIRE(cat_init(&loopDev) == 0);
    cat_debugRegister(testCmd);

    SECTION("Reads and writes reach the chip from the RTX thread")
    {
        RtxThread rtx;

        REQUIRE(answers("ZZT110 3F;ZZT110;", "ZZT110 3F;"));
        REQUIRE(chipRegs[0x10] == 0x3F);
    }

    SECTION("A call the RTX thread never takes times out")
    {
        REQUIRE(answers("ZZT110;", "?;"));

        // Nothing left queued for a late RTX thread
        chipRegs[0x10] = 0;
        cat_debugService();
        REQUIRE(chipRegs[0x10] == 0);
    }
}

TEST_CASE("CAT debug lines go out with the answers", "[cat][debug]")
{
    static constexpr char LINE[] = "T2 0123456789012345678901234567890123456789"
                                   "0123456789012345678901234567890123456789";

    REQUIRE(cat_init(&loopDev) == 0);

    SECTION("Queued lines after the answers")
    {
        cat_debugEmit("T2 A", 4);
        cat_debugEmit("T2 B", 4);

        REQUIRE(answers("ZZT9;", "?;ZZT2 A;ZZT2 B;"));
        REQUIRE(answers("ZZT9;", "?;"));
    }

    SECTION("Lines that do not fit are dropped and counted")
    {
        static constexpr size_t LEN = sizeof(LINE) - 1;
        Text expected = {};

        // 47 lines fit in 4096 characters, each sent as ZZ<line>;
        for (size_t i = 0; i < 50; i++)
            cat_debugEmit(LINE, LEN);

        for (size_t i = 0; i < 47; i++) {
            strcat(expected.data(), "ZZ");
            strcat(expected.data(), LINE);
            strcat(expected.data(), ";");
        }

        strcat(expected.data(), "ZZ!00003;");
        REQUIRE(answers("", expected.data()));
    }
}
