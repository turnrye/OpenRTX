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
    txActive(false), locked(false), txIdle(0)
{
}

OpMode_Modem::~OpMode_Modem()
{
}

void OpMode_Modem::enable()
{
    modem.init();
    txQueue.reset();
    rxQueue.reset();
    startRx = true;
    rxActive = false;
    txActive = false;
    locked = false;
    txIdle = 0;
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
    if (!txQueue.empty() && (status->txDisable == 0)) {
        status->opStatus = TX;
        return;
    }

    if (startRx) {
        status->opStatus = RX;
        return;
    }

    sleepFor(0, 30);
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

        // A full queue means the host fell behind: drop the oldest frame so
        // that it keeps seeing the latest state of the channel.
        if (rxQueue.tryPush(frame) == -EAGAIN) {
            struct modemFrame discard;
            rxQueue.tryPop(discard);
            rxQueue.tryPush(frame);
        }
    }

    bool lock = modem.rxLocked();
    if (locked && !lock)
        modem.clearStatus(status);
    locked = lock;

    // Switch to transmission as soon as the host has something to send.
    if (!txQueue.empty() && (status->txDisable == 0)) {
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
        txIdle = 0;
    }

    if (txQueue.tryPop(frame) == 0) {
        modem.txFrame(frame);
        modem.inspect(frame, status);
        txIdle = 0;
        return;
    }

    // Nothing to send: hold the carrier for a short while, the host may
    // just be late, then go back to listening.
    txIdle += 1;
    if (txIdle < TX_HANG_TICKS) {
        sleepFor(0, TX_IDLE_TICK_MS);
        return;
    }

    modem.stopTx();
    modem.clearStatus(status);
    txActive = false;
    startRx = true;
    status->opStatus = OFF;
}

int OpMode_Modem::modemPushTxFrame(const struct modemFrame *frame)
{
    if (frame->len > MODEM_FRAME_MAX_LEN)
        return -EINVAL;

    int ret = txQueue.tryPush(*frame);
    if (ret != 0)
        return -EAGAIN;

    return 0;
}

int OpMode_Modem::modemPopRxFrame(struct modemFrame *frame)
{
    int ret = rxQueue.tryPop(*frame);
    if (ret != 0)
        return -EAGAIN;

    return 0;
}

size_t OpMode_Modem::modemTxSpace()
{
    return TX_SLOTS - txQueue.size();
}
