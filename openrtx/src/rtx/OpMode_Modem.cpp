/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rtx/OpMode_Modem.hpp"
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include <errno.h>

OpMode_Modem::OpMode_Modem(FrameModem &modem, const enum opmode protocol) :
    modem(modem), protocol(protocol), startRx(false), rxActive(false),
    txActive(false), locked(false), txPending(false), txEnding(false),
    txPendingTick(0), lastFrameTick(0)
{
}

OpMode_Modem::~OpMode_Modem()
{
}

void OpMode_Modem::enable()
{
    modem.init();
    txQueue.clear();
    startRx = true;
    rxActive = false;
    txActive = false;
    locked = false;
    txPending = false;
    txEnding = false;
}

void OpMode_Modem::disable()
{
    startRx = false;
    platform_ledOff(GREEN);
    platform_ledOff(RED);

    if (rxActive)
        modem.stopRx();

    if (txActive)
        modem.stopTx();

    rxActive = false;
    txActive = false;
    locked = false;
    radio_disableRtx();
    modem.terminate();
}

void OpMode_Modem::update(rtxStatus_t *const status, const bool newCfg)
{
    (void)newCfg;

    // A hard TX lockout drops what the host sends, so that nothing stale goes
    // out once it is lifted.
    if (status->txDisable != 0)
        txQueue.clear();

    switch (status->opStatus) {
        case OFF:
            offState(status);
            break;

        case RX:
            rxState(status);
            break;

        case TX:
            txState(status);
            break;

        default:
            break;
    }

    switch (status->opStatus) {
        case RX:
            if (locked)
                platform_ledOn(GREEN);
            else
                platform_ledOff(GREEN);
            break;

        case TX:
            platform_ledOff(GREEN);
            platform_ledOn(RED);
            break;

        default:
            platform_ledOff(GREEN);
            platform_ledOff(RED);
            break;
    }
}

void OpMode_Modem::offState(rtxStatus_t *const status)
{
    radio_disableRtx();

    // Frames waiting from the host take precedence over listening.
    if (txReady() && (status->txDisable == 0)) {
        status->opStatus = TX;
        return;
    }

    if (startRx) {
        status->opStatus = RX;
        return;
    }

    sleepMs(30);
}

void OpMode_Modem::rxState(rtxStatus_t *const status)
{
    if (startRx) {
        modem.startRx();
        radio_enableRx();
        startRx = false;
        rxActive = true;
    }

    if (modem.rxUpdate(frame)) {
        modem.inspect(frame, status);

        // If the host has fallen behind and the queue is full the frame is
        // dropped: only the host side takes frames out.
        rxQueue.push(frame);
    }

    bool lock = modem.rxLocked();
    if (locked && !lock)
        modem.clearStatus(status);
    locked = lock;

    // Switch to transmission once the host has queued enough to send.
    if (txReady() && (status->txDisable == 0)) {
        modem.stopRx();
        rxActive = false;
        if (locked)
            modem.clearStatus(status);
        locked = false;
        status->opStatus = OFF;
    }
}

void OpMode_Modem::txState(rtxStatus_t *const status)
{
    if (!txActive) {
        radio_enableTx();
        modem.startTx();
        txActive = true;
        txPending = false;
        txEnding = false;
        lastFrameTick = nowMs();
    }

    bool keyDown = false;

    if (status->txDisable != 0) {
        // Hard TX lockout: stop now. update() has dropped the queue.
        keyDown = true;
    } else if (txQueue.pop(frame)) {
        modem.txFrame(frame);
        modem.inspect(frame, status);
        txEnding = modem.endsTransmission(frame);
        lastFrameTick = nowMs();
    } else {
        keyDown = holdCarrier();
    }

    if (!keyDown)
        return;

    modem.stopTx();
    modem.clearStatus(status);
    txActive = false;
    startRx = true;
    status->opStatus = OFF;
}

bool OpMode_Modem::holdCarrier()
{
    uint32_t limit = txEnding ? TX_TAIL_MS : TX_HANG_MS;
    if ((nowMs() - lastFrameTick) >= limit)
        return true;

    // The filler blocks for a frame time; when it did not (no filler,
    // output not running) pause anyway so that other threads run.
    uint32_t start = nowMs();
    modem.txIdle();
    if ((nowMs() - start) < TX_IDLE_TICK_MS)
        sleepMs(TX_IDLE_TICK_MS);

    return false;
}

int OpMode_Modem::modemPushTxFrame(const struct modemFrame *frame)
{
    if (frame->len > MODEM_FRAME_MAX_LEN)
        return -EINVAL;

    if (!txQueue.push(*frame))
        return -EAGAIN;

    return 0;
}

bool OpMode_Modem::txReady()
{
    size_t queued = txQueue.size();

    if (queued == 0) {
        txPending = false;
        return false;
    }

    if (!txPending) {
        txPending = true;
        txPendingTick = nowMs();
    }

    if (queued >= TX_START_FRAMES)
        return true;

    return (nowMs() - txPendingTick) >= TX_START_WAIT_MS;
}

int OpMode_Modem::modemPopRxFrame(struct modemFrame *frame)
{
    if (!rxQueue.pop(*frame))
        return -EAGAIN;

    return 0;
}

size_t OpMode_Modem::modemTxSpace()
{
    return TX_SLOTS - txQueue.size();
}

uint32_t OpMode_Modem::nowMs()
{
    return (uint32_t)getTick();
}

void OpMode_Modem::sleepMs(const uint32_t ms)
{
    sleepFor(0, ms);
}
