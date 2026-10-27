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

    void txIdle() override
    {
        idles++;
        if (idleBlocks)
            clock += IDLE_MS;
    }

    bool endsTransmission(const struct modemFrame &frame) override
    {
        return frame.type == EOT_TYPE;
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
    int idles = 0;
    int clears = 0;
    bool idleBlocks = true; ///< txIdle() takes a frame time, as on hardware.
    uint32_t clock = 1000;  ///< Simulated time, in milliseconds.

    static constexpr uint8_t EOT_TYPE = 0xEE; ///< Frame type ending a TX.
    static constexpr uint32_t IDLE_MS = 40;   ///< Time taken by txIdle().
};

/**
 * OpMode_Modem on the fake modem's simulated clock: sleeping advances the
 * time instead of blocking, so the timing rules are checked exactly.
 */
class TestMode : public OpMode_Modem
{
public:
    explicit TestMode(FakeModem &modem) :
        OpMode_Modem(modem, OPMODE_M17), clock(modem.clock)
    {
    }

protected:
    uint32_t nowMs() override
    {
        return clock;
    }

    void sleepMs(const uint32_t ms) override
    {
        clock += ms;
    }

private:
    uint32_t &clock;
};

/**
 * Bring a freshly enabled mode to receiving, with sampling started.
 */
void startReceiving(TestMode &mode, rtxStatus_t &status)
{
    mode.enable();
    mode.update(&status, false); // OFF -> RX
    mode.update(&status, false); // RX, sampling started
}

/**
 * Run the mode until it leaves TX, with a bound in case it never does.
 */
void runUntilKeyDown(TestMode &mode, rtxStatus_t &status)
{
    for (int i = 0; (i < 1000) && (status.opStatus == TX); i++)
        mode.update(&status, false);
}

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
    TestMode mode(modem);

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
    TestMode mode(modem);
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
    TestMode mode(modem);
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
    TestMode mode(modem);
    rtxStatus_t status = makeStatus();

    startReceiving(mode, status);
    REQUIRE(modem.rxRunning);

    struct modemFrame a = makeFrame(0, 0xA0);
    struct modemFrame b = makeFrame(1, 0xB0);
    struct modemFrame c = makeFrame(1, 0xC0);
    REQUIRE(mode.modemPushTxFrame(&a) == 0);
    REQUIRE(mode.modemPushTxFrame(&b) == 0);

    // Two frames are not enough to key up yet.
    mode.update(&status, false);
    REQUIRE(status.opStatus == RX);
    REQUIRE(modem.rxRunning);

    // The third one is: RX leaves through OFF.
    REQUIRE(mode.modemPushTxFrame(&c) == 0);
    mode.update(&status, false);
    REQUIRE(status.opStatus == OFF);
    REQUIRE(modem.rxRunning == false);

    mode.update(&status, false);
    REQUIRE(status.opStatus == TX);

    mode.update(&status, false);
    REQUIRE(modem.txStarts == 1);
    REQUIRE(modem.sent.size() == 1);
    REQUIRE(modem.sent[0].data[0] == 0xA0);
    REQUIRE(mode.modemTxSpace() == OpMode_Modem::TX_SLOTS - 2);

    mode.update(&status, false);
    REQUIRE(modem.sent.size() == 2);
    REQUIRE(modem.sent[1].data[0] == 0xB0);
    mode.update(&status, false);
    REQUIRE(modem.sent.size() == 3);
    REQUIRE(modem.sent[2].data[0] == 0xC0);
    REQUIRE(mode.modemTxSpace() == OpMode_Modem::TX_SLOTS);

    // Nothing more from the host: the filler holds the carrier for exactly
    // the hang time, then the modem goes back to RX.
    uint32_t lastFrame = modem.clock;
    runUntilKeyDown(mode, status);

    REQUIRE(modem.idles == OpMode_Modem::TX_HANG_MS / FakeModem::IDLE_MS);
    REQUIRE(modem.clock - lastFrame == OpMode_Modem::TX_HANG_MS);
    REQUIRE(modem.txStops == 1);
    REQUIRE(status.opStatus == OFF);

    mode.update(&status, false);
    REQUIRE(status.opStatus == RX);
    mode.update(&status, false);
    REQUIRE(modem.rxRunning);
}

TEST_CASE("OpMode_Modem: without a filler the hang time is waited out",
          "[modem][opmode]")
{
    FakeModem modem;
    TestMode mode(modem);
    rtxStatus_t status = makeStatus();
    modem.idleBlocks = false;

    startReceiving(mode, status);
    for (uint8_t i = 0; i < OpMode_Modem::TX_START_FRAMES; i++) {
        struct modemFrame a = makeFrame(0, 0xA0 + i);
        REQUIRE(mode.modemPushTxFrame(&a) == 0);
    }

    mode.update(&status, false); // RX -> OFF
    mode.update(&status, false); // OFF -> TX
    for (uint8_t i = 0; i < OpMode_Modem::TX_START_FRAMES; i++)
        mode.update(&status, false);

    uint32_t lastFrame = modem.clock;
    runUntilKeyDown(mode, status);

    REQUIRE(modem.idles
            == OpMode_Modem::TX_HANG_MS / OpMode_Modem::TX_IDLE_TICK_MS);
    REQUIRE(modem.clock - lastFrame == OpMode_Modem::TX_HANG_MS);
    REQUIRE(status.opStatus == OFF);
}

TEST_CASE("OpMode_Modem: a lone frame is transmitted once it has waited",
          "[modem][opmode]")
{
    FakeModem modem;
    TestMode mode(modem);
    rtxStatus_t status = makeStatus();

    startReceiving(mode, status);

    struct modemFrame a = makeFrame(0, 0xA0);
    REQUIRE(mode.modemPushTxFrame(&a) == 0);
    mode.update(&status, false);
    REQUIRE(status.opStatus == RX);

    // The wait counts from when the rtx thread first saw the frame.
    modem.clock += OpMode_Modem::TX_START_WAIT_MS - 1;
    mode.update(&status, false);
    REQUIRE(status.opStatus == RX);

    modem.clock += 1;
    mode.update(&status, false);
    REQUIRE(status.opStatus == OFF);
    mode.update(&status, false);
    REQUIRE(status.opStatus == TX);
    mode.update(&status, false);
    REQUIRE(modem.sent.size() == 1);
}

TEST_CASE("OpMode_Modem: an end of transmission frame keys down after a "
          "short tail",
          "[modem][opmode]")
{
    FakeModem modem;
    TestMode mode(modem);
    rtxStatus_t status = makeStatus();

    startReceiving(mode, status);

    struct modemFrame a = makeFrame(0, 0xA0);
    struct modemFrame b = makeFrame(1, 0xB0);
    struct modemFrame eot = makeFrame(FakeModem::EOT_TYPE, 0x55);
    REQUIRE(mode.modemPushTxFrame(&a) == 0);
    REQUIRE(mode.modemPushTxFrame(&b) == 0);
    REQUIRE(mode.modemPushTxFrame(&eot) == 0);

    mode.update(&status, false); // RX -> OFF
    mode.update(&status, false); // OFF -> TX
    for (int i = 0; i < 3; i++)
        mode.update(&status, false);
    REQUIRE(modem.sent.size() == 3);

    uint32_t lastFrame = modem.clock;
    runUntilKeyDown(mode, status);

    REQUIRE(modem.idles == OpMode_Modem::TX_TAIL_MS / FakeModem::IDLE_MS);
    REQUIRE(modem.clock - lastFrame == OpMode_Modem::TX_TAIL_MS);
    REQUIRE(modem.txStops == 1);
    REQUIRE(status.opStatus == OFF);
}

TEST_CASE("OpMode_Modem: a transmission following an end frame shares the "
          "carrier",
          "[modem][opmode]")
{
    FakeModem modem;
    TestMode mode(modem);
    rtxStatus_t status = makeStatus();

    startReceiving(mode, status);

    struct modemFrame eot = makeFrame(FakeModem::EOT_TYPE, 0x55);
    for (uint8_t i = 0; i < 2; i++) {
        struct modemFrame a = makeFrame(0, 0xA0 + i);
        REQUIRE(mode.modemPushTxFrame(&a) == 0);
    }
    REQUIRE(mode.modemPushTxFrame(&eot) == 0);

    mode.update(&status, false); // RX -> OFF
    mode.update(&status, false); // OFF -> TX
    for (int i = 0; i < 3; i++)
        mode.update(&status, false);

    // The next transmission arrives within the tail.
    mode.update(&status, false);
    struct modemFrame next = makeFrame(0, 0xD0);
    REQUIRE(mode.modemPushTxFrame(&next) == 0);
    mode.update(&status, false);

    REQUIRE(modem.sent.size() == 4);
    REQUIRE(modem.sent[3].data[0] == 0xD0);
    REQUIRE(modem.txStarts == 1);
    REQUIRE(status.opStatus == TX);
}

TEST_CASE("OpMode_Modem: a TX lockout ends a transmission and drops the "
          "queue",
          "[modem][opmode]")
{
    FakeModem modem;
    TestMode mode(modem);
    rtxStatus_t status = makeStatus();

    startReceiving(mode, status);
    for (uint8_t i = 0; i < OpMode_Modem::TX_START_FRAMES + 2; i++) {
        struct modemFrame a = makeFrame(0, 0xA0 + i);
        REQUIRE(mode.modemPushTxFrame(&a) == 0);
    }

    mode.update(&status, false); // RX -> OFF
    mode.update(&status, false); // OFF -> TX
    mode.update(&status, false); // first frame
    REQUIRE(modem.sent.size() == 1);

    // Frames are still queued, but the lockout wins and drops them.
    status.txDisable = 1;
    mode.update(&status, false);
    REQUIRE(modem.sent.size() == 1);
    REQUIRE(modem.txStops == 1);
    REQUIRE(status.opStatus == OFF);
    REQUIRE(mode.modemTxSpace() == OpMode_Modem::TX_SLOTS);

    // Nothing stale goes out once the lockout is lifted.
    status.txDisable = 0;
    for (int i = 0; i < 3; i++)
        mode.update(&status, false);
    REQUIRE(status.opStatus == RX);
    REQUIRE(modem.txStarts == 1);
    REQUIRE(modem.sent.size() == 1);
}

TEST_CASE("OpMode_Modem: hard TX lockout keeps the modem receiving",
          "[modem][opmode]")
{
    FakeModem modem;
    TestMode mode(modem);
    rtxStatus_t status = makeStatus();
    status.txDisable = 1;

    startReceiving(mode, status);
    for (uint8_t i = 0; i < OpMode_Modem::TX_START_FRAMES; i++) {
        struct modemFrame a = makeFrame(0, 0xA0 + i);
        REQUIRE(mode.modemPushTxFrame(&a) == 0);
    }

    for (int i = 0; i < 3; i++)
        mode.update(&status, false);

    REQUIRE(status.opStatus == RX);
    REQUIRE(modem.txStarts == 0);
    REQUIRE(mode.modemTxSpace() == OpMode_Modem::TX_SLOTS);
}
