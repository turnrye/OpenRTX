/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The DMR debug tap against a fake USB serial link and clock: what it writes
 * to the host, the commands it takes from it, and how it copes with more
 * output than its buffer holds.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "core/dmr_debug.h"
#include "interfaces/delays.h"
#include "interfaces/usb_serial.h"
}

/* ---------------------------------------------------------------- the fakes */

static long long tick;
static std::string toHost;   // Everything written to the host
static std::string fromHost; // Waiting to be read by the tap

extern "C" {

long long getTick()
{
    return tick;
}

ssize_t usb_serial_write(const void *buf, size_t len)
{
    toHost.append((const char *)buf, len);
    return len;
}

ssize_t usb_serial_read(void *buf, size_t len)
{
    size_t n = std::min(len, fromHost.size());
    memcpy(buf, fromHost.data(), n);
    fromHost.erase(0, n);
    return n;
}

} // extern "C"

/* ---------------------------------------------------------------- scenarios */

static int runs;
static void command()
{
    runs++;
}

static int otherRuns;
static void otherCommand()
{
    otherRuns++;
}

/* Start the tap from scratch, at the given tick, and drop its banner. */
static void startTap(long long at = 1000)
{
    tick = at;
    toHost.clear();
    fromHost.clear();
    for (char key : { 'a', 'b', 'c', 'd', 'e', 'm', 'x' })
        dmrDebug_setCommand(key, NULL, NULL);
    runs = otherRuns = 0;
    dmrDebug_init();
    dmrDebug_task();
    toHost.clear();
}

/* Send keys from the host and flush what the tap says back. */
static std::string send(const std::string &keys)
{
    fromHost += keys;
    dmrDebug_task(); // Takes the keys
    dmrDebug_task(); // Sends what they logged
    std::string out = toHost;
    toHost.clear();
    return out;
}

static std::string flush()
{
    dmrDebug_task();
    std::string out = toHost;
    toHost.clear();
    return out;
}

TEST_CASE("DMR tap starts with the info channel and says so", "[dmr][tap]")
{
    tick = 5000;
    toHost.clear();
    dmrDebug_init();
    std::string out = flush();
    REQUIRE(out.find("[00000000] tap: DMR debug tap ready") == 0);
    REQUIRE(dmrDebug_channelEnabled(DMR_DBG_INFO));
    REQUIRE_FALSE(dmrDebug_channelEnabled(DMR_DBG_IRQ));
    REQUIRE_FALSE(dmrDebug_channelEnabled(DMR_DBG_LC));
    REQUIRE_FALSE(dmrDebug_channelEnabled(DMR_DBG_VP));
}

TEST_CASE("DMR tap stamps lines with the time since it started", "[dmr][tap]")
{
    startTap(1000);
    tick = 1234;
    dmrDebug_log(DMR_DBG_INFO, "call: from %d", 7);
    REQUIRE(flush() == "[00000234] call: from 7\r\n");

    // Lines on a channel that is off go nowhere
    dmrDebug_log(DMR_DBG_IRQ, "irq: timeslot 33");
    REQUIRE(flush().empty());
}

TEST_CASE("DMR tap channels toggle from the host", "[dmr][tap]")
{
    startTap();
    REQUIRE(send("2").find("tap: channels 03") != std::string::npos);
    REQUIRE(dmrDebug_channelEnabled(DMR_DBG_IRQ));
    REQUIRE(send("34").find("tap: channels 0F") != std::string::npos);
    REQUIRE(send("2").find("tap: channels 0D") != std::string::npos);
    REQUIRE_FALSE(dmrDebug_channelEnabled(DMR_DBG_IRQ));
}

TEST_CASE("DMR tap records carry tag, time, payload and checksum", "[dmr][tap]")
{
    startTap(1000);
    const uint8_t payload[] = { 0x01, 0xAB };

    // Off by default
    dmrDebug_record(DMR_DBG_VP, "VP", payload, sizeof(payload));
    REQUIRE(flush().empty());

    send("4");
    tick = 1042;
    dmrDebug_record(DMR_DBG_VP, "VP", payload, sizeof(payload));
    REQUIRE(flush() == "$VP 00000042 01AB AC\r\n");

    // Longer than a record can be: refused with a note
    uint8_t big[49] = { 0 };
    dmrDebug_record(DMR_DBG_VP, "VP", big, sizeof(big));
    REQUIRE(flush().find("tap: record VP too long (49 bytes)")
            != std::string::npos);
}

TEST_CASE("DMR tap runs and lists the commands drivers register", "[dmr][tap]")
{
    startTap();
    REQUIRE(dmrDebug_setCommand('m', command, "tx test pattern") == 0);

    send("m");
    REQUIRE(runs == 1);
    REQUIRE(send("?").find("tap: m tx test pattern") != std::string::npos);

    // The same key again replaces the command rather than adding one
    REQUIRE(dmrDebug_setCommand('m', otherCommand, "other") == 0);
    send("m");
    REQUIRE(runs == 1);
    REQUIRE(otherRuns == 1);

    // Removed, the key does nothing
    dmrDebug_setCommand('m', NULL, NULL);
    send("m");
    REQUIRE(otherRuns == 1);
    REQUIRE(send("?").find("tap: m") == std::string::npos);
}

TEST_CASE("DMR tap holds four commands", "[dmr][tap]")
{
    startTap();
    REQUIRE(dmrDebug_setCommand('a', command, "a") == 0);
    REQUIRE(dmrDebug_setCommand('b', command, "b") == 0);
    REQUIRE(dmrDebug_setCommand('c', command, "c") == 0);
    REQUIRE(dmrDebug_setCommand('d', command, "d") == 0);
    REQUIRE(dmrDebug_setCommand('e', command, "e") == -1);

    // Freeing one makes room
    dmrDebug_setCommand('b', NULL, NULL);
    REQUIRE(dmrDebug_setCommand('e', command, "e") == 0);
}

TEST_CASE("DMR tap drops whole lines when its buffer fills, and says so",
          "[dmr][tap]")
{
    startTap();
    std::string line(60, 'x');
    for (int n = 0; n < 60; n++) // More than the 2kB buffer holds
        dmrDebug_log(DMR_DBG_INFO, "%s", line.c_str());

    std::string out;
    for (int n = 0; n < 20; n++)
        out += flush();

    // Every line that went out is whole
    size_t lines = 0;
    size_t pos = 0;
    while ((pos = out.find("\r\n", pos)) != std::string::npos) {
        pos += 2;
        lines++;
    }
    REQUIRE(out.size() >= 2);
    REQUIRE(out.substr(out.size() - 2) == "\r\n");
    REQUIRE(lines < 60);
    REQUIRE(out.find("tap: dropped ") != std::string::npos);
}

TEST_CASE("DMR tap markers go out straight away", "[dmr][tap]")
{
    startTap();
    dmrDebug_mark("dmr: configuring baseband");
    REQUIRE(toHost == "<< dmr: configuring baseband\r\n");
}
