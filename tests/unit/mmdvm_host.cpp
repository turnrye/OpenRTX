/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * Unit tests for the MMDVM host protocol handler, driven through a fake
 * character device and the rtx frame port stub.
 */

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cstring>
#include <deque>
#include <vector>
#include "core/mmdvm_host.h"
#include "core/state.h"
#include "protocols/M17/m17.h"
#include "rtx_modem_stub.h"

namespace
{

std::deque<uint8_t> fromHost;
std::vector<uint8_t> toHost;
int inits = 0;

int fakeInit(const struct chardev *dev)
{
    (void)dev;
    inits++;
    return 0;
}

void fakeTerminate(const struct chardev *dev)
{
    (void)dev;
}

ssize_t fakeRead(const struct chardev *dev, void *buf, size_t len)
{
    (void)dev;
    uint8_t *out = static_cast<uint8_t *>(buf);
    size_t n = 0;

    while ((n < len) && !fromHost.empty()) {
        out[n++] = fromHost.front();
        fromHost.pop_front();
    }

    return n;
}

ssize_t fakeWrite(const struct chardev *dev, const void *buf, size_t len)
{
    (void)dev;
    const uint8_t *in = static_cast<const uint8_t *>(buf);
    toHost.insert(toHost.end(), in, in + len);
    return len;
}

const struct chardevApi fakeApi = { fakeInit, fakeTerminate, fakeRead,
                                    fakeWrite };
const struct chardev fakeDev = { &fakeApi, nullptr, nullptr };

void setup()
{
    fromHost.clear();
    toHost.clear();
    rtxStub_reset();
    memset(&state, 0, sizeof(state));
    mmdvmHost_init(&fakeDev);
}

void hostSends(std::vector<uint8_t> bytes)
{
    fromHost.insert(fromHost.end(), bytes.begin(), bytes.end());
    mmdvmHost_task();
}

std::vector<uint8_t> frameFromHost(uint8_t type, uint8_t fill)
{
    std::vector<uint8_t> f = { 0xE0, 52, type, 0x00 };
    f.insert(f.end(), 48, fill);
    return f;
}

} // namespace

TEST_CASE("MMDVM host: nothing is sent before the host speaks", "[mmdvm][host]")
{
    setup();
    REQUIRE(inits >= 1);

    struct modemFrame f = {};
    f.type = M17_MODEM_STREAM;
    f.len = 48;
    rtxStub.toPop.push_back(f);

    mmdvmHost_task();
    REQUIRE(toHost.empty());
    REQUIRE(mmdvmHost_connected() == false);
}

TEST_CASE("MMDVM host: GET_VERSION answers protocol version 1", "[mmdvm][host]")
{
    setup();
    hostSends({ 0xE0, 0x03, 0x00 });

    REQUIRE(mmdvmHost_connected());
    REQUIRE(toHost.size() > 4);
    REQUIRE(toHost[0] == 0xE0);
    REQUIRE(toHost[1] == toHost.size());
    REQUIRE(toHost[2] == 0x00);
    REQUIRE(toHost[3] == 0x01);

    std::string desc(toHost.begin() + 4, toHost.end());
    REQUIRE(desc.rfind("OpenRTX Test", 0) == 0);
}

TEST_CASE("MMDVM host: GET_STATUS reports mode, flags and M17 space",
          "[mmdvm][host]")
{
    setup();
    rtxStub.status.opMode = OPMODE_M17;
    rtxStub.status.hostModem = 1;
    rtxStub.status.opStatus = TX;
    rtxStub.txSpace = 3;
    rtxStub.squelchOpen = true;

    hostSends({ 0xE0, 0x03, 0x01 });

    REQUIRE(toHost.size() == 14);
    REQUIRE(toHost[1] == 14);
    REQUIRE(toHost[2] == 0x01);
    REQUIRE((toHost[3] & 0x80) == 0x80); // M17 enabled
    REQUIRE(toHost[4] == 7);             // mode M17
    REQUIRE((toHost[5] & 0x01) == 0x01); // TX
    REQUIRE((toHost[5] & 0x40) == 0x40); // carrier detect
    REQUIRE(toHost[13] == 3);

    // Not in host-driven mode: idle, no space.
    toHost.clear();
    rtxStub.status.hostModem = 0;
    rtxStub.status.opStatus = RX;
    rtxStub.txSpace = 0;
    hostSends({ 0xE0, 0x03, 0x01 });
    REQUIRE(toHost[4] == 0);
    REQUIRE(toHost[13] == 0);
}

TEST_CASE("MMDVM host: SET_CONFIG, SET_MODE and SET_FREQ are acknowledged",
          "[mmdvm][host]")
{
    setup();

    std::vector<uint8_t> cfg(26, 0);
    cfg[0] = 0xE0;
    cfg[1] = 26;
    cfg[2] = 0x02;
    cfg[4] = 0x40; // M17 enable
    cfg[5] = 10;   // TX delay
    hostSends(cfg);
    REQUIRE(toHost == std::vector<uint8_t>({ 0xE0, 0x04, 0x70, 0x02 }));

    toHost.clear();
    hostSends({ 0xE0, 0x04, 0x03, 0x07 });
    REQUIRE(toHost == std::vector<uint8_t>({ 0xE0, 0x04, 0x70, 0x03 }));

    toHost.clear();
    hostSends({ 0xE0, 0x04, 0x03, 0x02 }); // DMR: not available
    REQUIRE(toHost == std::vector<uint8_t>({ 0xE0, 0x05, 0x7F, 0x03, 0x04 }));

    toHost.clear();
    uint32_t rx = 433475000;
    uint32_t tx = 438075000;
    std::vector<uint8_t> freq = { 0xE0, 12, 0x04, 0x00 };
    for (int i = 0; i < 4; i++)
        freq.push_back((rx >> (8 * i)) & 0xFF);
    for (int i = 0; i < 4; i++)
        freq.push_back((tx >> (8 * i)) & 0xFF);
    hostSends(freq);
    REQUIRE(toHost == std::vector<uint8_t>({ 0xE0, 0x04, 0x70, 0x04 }));
    REQUIRE(state.channel.rx_frequency == rx);
    REQUIRE(state.channel.tx_frequency == tx);
    REQUIRE(state.rtxCfgDirty);
}

TEST_CASE("MMDVM host: unknown commands get a NAK", "[mmdvm][host]")
{
    setup();
    hostSends({ 0xE0, 0x03, 0x10 }); // D-Star header
    REQUIRE(toHost == std::vector<uint8_t>({ 0xE0, 0x05, 0x7F, 0x10, 0x01 }));
}

TEST_CASE("MMDVM host: M17 frames from the host reach the frame port",
          "[mmdvm][host]")
{
    setup();
    hostSends(frameFromHost(0x45, 0xA1));
    hostSends(frameFromHost(0x46, 0xB2));
    hostSends(frameFromHost(0x47, 0xC3));
    hostSends(frameFromHost(0x49, 0xD4));

    REQUIRE(toHost.empty());
    REQUIRE(rtxStub.pushed.size() == 4);
    REQUIRE(rtxStub.pushed[0].type == M17_MODEM_LSF);
    REQUIRE(rtxStub.pushed[0].len == 48);
    REQUIRE(rtxStub.pushed[0].data[0] == 0xA1);
    REQUIRE(rtxStub.pushed[0].data[47] == 0xA1);
    REQUIRE(rtxStub.pushed[1].type == M17_MODEM_STREAM);
    REQUIRE(rtxStub.pushed[2].type == M17_MODEM_PACKET);
    REQUIRE(rtxStub.pushed[3].type == M17_MODEM_EOT);

    // A bare EOT is turned into a full on-air EOT frame.
    hostSends({ 0xE0, 0x03, 0x49 });
    REQUIRE(rtxStub.pushed.size() == 5);
    REQUIRE(rtxStub.pushed[4].type == M17_MODEM_EOT);
    REQUIRE(rtxStub.pushed[4].data[0] == 0x55);
    REQUIRE(rtxStub.pushed[4].data[1] == 0x5D);
    REQUIRE(rtxStub.pushed[4].data[47] == 0x5D);

    // Frames are split across reads without losing sync.
    std::vector<uint8_t> f = frameFromHost(0x46, 0xEE);
    fromHost.insert(fromHost.end(), f.begin(), f.begin() + 20);
    mmdvmHost_task();
    REQUIRE(rtxStub.pushed.size() == 5);
    fromHost.insert(fromHost.end(), f.begin() + 20, f.end());
    mmdvmHost_task();
    REQUIRE(rtxStub.pushed.size() == 6);
    REQUIRE(rtxStub.pushed[5].data[10] == 0xEE);
}

TEST_CASE("MMDVM host: a full transmit queue is reported with a NAK",
          "[mmdvm][host]")
{
    setup();
    rtxStub.pushResult = -EAGAIN;
    hostSends(frameFromHost(0x46, 0x00));
    REQUIRE(toHost == std::vector<uint8_t>({ 0xE0, 0x05, 0x7F, 0x46, 0x05 }));

    toHost.clear();
    rtxStub.pushResult = -ENOTSUP;
    hostSends(frameFromHost(0x46, 0x00));
    REQUIRE(toHost == std::vector<uint8_t>({ 0xE0, 0x05, 0x7F, 0x46, 0x04 }));
}

TEST_CASE("MMDVM host: received frames are forwarded with the sync flag",
          "[mmdvm][host]")
{
    setup();
    hostSends({ 0xE0, 0x03, 0x01 }); // make the host known
    toHost.clear();

    struct modemFrame lsf = {};
    lsf.type = M17_MODEM_LSF;
    lsf.flags = MODEM_FRAME_FIRST;
    lsf.len = 48;
    memset(lsf.data, 0x5A, sizeof(lsf.data));

    struct modemFrame stream = lsf;
    stream.type = M17_MODEM_STREAM;
    stream.flags = 0;

    struct modemFrame lost = {};
    lost.type = M17_MODEM_LOST;

    rtxStub.toPop.push_back(lsf);
    rtxStub.toPop.push_back(stream);
    rtxStub.toPop.push_back(lost);
    mmdvmHost_task();

    REQUIRE(toHost.size() == 52 + 52 + 3);
    REQUIRE(toHost[0] == 0xE0);
    REQUIRE(toHost[1] == 52);
    REQUIRE(toHost[2] == 0x45);
    REQUIRE(toHost[3] == 0x01);
    REQUIRE(toHost[4] == 0x5A);
    REQUIRE(toHost[51] == 0x5A);
    REQUIRE(toHost[52 + 2] == 0x46);
    REQUIRE(toHost[52 + 3] == 0x00);
    REQUIRE(toHost[104] == 0xE0);
    REQUIRE(toHost[105] == 3);
    REQUIRE(toHost[106] == 0x48);
}
