/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * Unit tests for OpMode_Modem, the host-driven operating mode, run against a
 * scripted FrameModem so that no baseband or radio hardware is involved.
 */

#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cstring>
#include <deque>
#include <vector>
#include "rtx/OpMode_Modem.hpp"

namespace
{

class FakeModem : public FrameModem
{
public:
    void init() override
    {
        inits++;
    }

    void terminate() override
    {
        terminates++;
    }

    void startRx() override
    {
        rxRunning = true;
    }

    void stopRx() override
    {
        rxRunning = false;
    }

    bool rxUpdate(struct modemFrame &frame) override
    {
        if (rxScript.empty())
            return false;

        frame = rxScript.front();
        rxScript.pop_front();
        return true;
    }

    bool rxLocked() override
    {
        return lock;
    }

    void startTx() override
    {
        txStarts++;
        txRunning = true;
    }

    void txFrame(const struct modemFrame &frame) override
    {
        sent.push_back(frame);
    }

    void stopTx() override
    {
        txStops++;
        txRunning = false;
    }

    void inspect(const struct modemFrame &frame,
                 rtxStatus_t *const status) override
    {
        (void)frame;
        status->lsfOk = true;
    }

    void clearStatus(rtxStatus_t *const status) override
    {
        status->lsfOk = false;
        clears++;
    }

    std::deque<struct modemFrame> rxScript;
    std::vector<struct modemFrame> sent;
    bool lock = false;
    bool rxRunning = false;
    bool txRunning = false;
    int inits = 0;
    int terminates = 0;
    int txStarts = 0;
    int txStops = 0;
    int clears = 0;
};

struct modemFrame makeFrame(uint8_t type, uint8_t fill)
{
    struct modemFrame f;
    f.type = type;
    f.flags = 0;
    f.len = MODEM_FRAME_MAX_LEN;
    memset(f.data, fill, sizeof(f.data));
    return f;
}

rtxStatus_t makeStatus()
{
    rtxStatus_t s;
    memset(&s, 0, sizeof(s));
    s.opMode = OPMODE_M17;
    s.opStatus = OFF;
    return s;
}

} // namespace

TEST_CASE("OpMode_Modem: transmit queue accounting", "[modem][opmode]")
{
    FakeModem modem;
    OpMode_Modem mode(modem, OPMODE_M17);

    REQUIRE(mode.getID() == OPMODE_M17);
    REQUIRE(mode.isHostDriven());
    REQUIRE(mode.modemTxSpace() == OpMode_Modem::TX_SLOTS);

    struct modemFrame f = makeFrame(0, 0xAA);
    for (size_t i = 0; i < OpMode_Modem::TX_SLOTS; i++) {
        REQUIRE(mode.modemPushTxFrame(&f) == 0);
        REQUIRE(mode.modemTxSpace() == OpMode_Modem::TX_SLOTS - i - 1);
    }

    REQUIRE(mode.modemPushTxFrame(&f) == -EAGAIN);

    f.len = MODEM_FRAME_MAX_LEN + 1;
    REQUIRE(mode.modemPushTxFrame(&f) == -EINVAL);

    struct modemFrame out;
    REQUIRE(mode.modemPopRxFrame(&out) == -EAGAIN);
}

TEST_CASE("OpMode_Modem: received frames reach the host in order",
          "[modem][opmode]")
{
    FakeModem modem;
    OpMode_Modem mode(modem, OPMODE_M17);
    rtxStatus_t status = makeStatus();

    mode.enable();
    REQUIRE(modem.inits == 1);

    // OFF -> RX
    mode.update(&status, false);
    REQUIRE(status.opStatus == RX);

    // First RX tick starts sampling and delivers one frame.
    modem.lock = true;
    modem.rxScript.push_back(makeFrame(1, 0x11));
    modem.rxScript.push_back(makeFrame(1, 0x22));
    mode.update(&status, false);
    REQUIRE(modem.rxRunning);
    REQUIRE(mode.rxSquelchOpen());
    REQUIRE(status.lsfOk);

    mode.update(&status, false);

    struct modemFrame out;
    REQUIRE(mode.modemPopRxFrame(&out) == 0);
    REQUIRE(out.data[0] == 0x11);
    REQUIRE(mode.modemPopRxFrame(&out) == 0);
    REQUIRE(out.data[0] == 0x22);
    REQUIRE(mode.modemPopRxFrame(&out) == -EAGAIN);

    // Losing lock clears the display status.
    modem.lock = false;
    mode.update(&status, false);
    REQUIRE(status.lsfOk == false);
    REQUIRE(modem.clears == 1);
    REQUIRE(mode.rxSquelchOpen() == false);

    mode.disable();
    REQUIRE(modem.terminates == 1);
    REQUIRE(modem.rxRunning == false);
}

TEST_CASE("OpMode_Modem: a full receive queue drops the oldest frame",
          "[modem][opmode]")
{
    FakeModem modem;
    OpMode_Modem mode(modem, OPMODE_M17);
    rtxStatus_t status = makeStatus();

    mode.enable();
    mode.update(&status, false);

    for (uint8_t i = 0; i < OpMode_Modem::RX_SLOTS + 2; i++) {
        modem.rxScript.push_back(makeFrame(1, i));
        mode.update(&status, false);
    }

    struct modemFrame out;
    REQUIRE(mode.modemPopRxFrame(&out) == 0);
    REQUIRE(out.data[0] == 2);
}

TEST_CASE("OpMode_Modem: host frames are transmitted then the modem returns "
          "to receive",
          "[modem][opmode]")
{
    FakeModem modem;
    OpMode_Modem mode(modem, OPMODE_M17);
    rtxStatus_t status = makeStatus();

    mode.enable();
    mode.update(&status, false); // OFF -> RX
    mode.update(&status, false); // RX tick, sampling started
    REQUIRE(modem.rxRunning);

    struct modemFrame a = makeFrame(0, 0xA0);
    struct modemFrame b = makeFrame(1, 0xB0);
    REQUIRE(mode.modemPushTxFrame(&a) == 0);
    REQUIRE(mode.modemPushTxFrame(&b) == 0);

    // RX notices the pending frames and leaves through OFF.
    mode.update(&status, false);
    REQUIRE(status.opStatus == OFF);
    REQUIRE(modem.rxRunning == false);

    mode.update(&status, false);
    REQUIRE(status.opStatus == TX);

    mode.update(&status, false);
    REQUIRE(modem.txStarts == 1);
    REQUIRE(modem.sent.size() == 1);
    REQUIRE(modem.sent[0].data[0] == 0xA0);
    REQUIRE(mode.modemTxSpace() == OpMode_Modem::TX_SLOTS - 1);

    mode.update(&status, false);
    REQUIRE(modem.sent.size() == 2);
    REQUIRE(modem.sent[1].data[0] == 0xB0);
    REQUIRE(mode.modemTxSpace() == OpMode_Modem::TX_SLOTS);

    // Nothing more from the host: hang time elapses, then back to RX.
    for (int i = 0; i < 12 && status.opStatus == TX; i++)
        mode.update(&status, false);

    REQUIRE(modem.txStops == 1);
    REQUIRE(status.opStatus == OFF);

    mode.update(&status, false);
    REQUIRE(status.opStatus == RX);
    mode.update(&status, false);
    REQUIRE(modem.rxRunning);
}

TEST_CASE("OpMode_Modem: hard TX lockout keeps the modem receiving",
          "[modem][opmode]")
{
    FakeModem modem;
    OpMode_Modem mode(modem, OPMODE_M17);
    rtxStatus_t status = makeStatus();
    status.txDisable = 1;

    mode.enable();
    mode.update(&status, false);
    mode.update(&status, false);

    struct modemFrame a = makeFrame(0, 0xA0);
    REQUIRE(mode.modemPushTxFrame(&a) == 0);

    for (int i = 0; i < 3; i++)
        mode.update(&status, false);

    REQUIRE(status.opStatus == RX);
    REQUIRE(modem.txStarts == 0);
}
