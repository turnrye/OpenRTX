/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "rtx/OpMode_APRS.hpp"
#include "rtx/rtx.h"

#include <cstring>
#include <errno.h>

OpMode_APRS::OpMode_APRS() :
    streamRunning(false), rxAudioPath(0), basebandId(0), currTxPkt(nullptr),
    txPhase(TxPhase::KEYUP)
{
    csma_init(&csma, CSMA_PERSIST, CSMA_SLOT_TIME, CSMA_DEFER_TIMEOUT, 1);
    aprsDigi_init(&digi, nullptr);
    memset(&digiDesc, 0, sizeof(digiDesc));
}

OpMode_APRS::~OpMode_APRS()
{
}

void OpMode_APRS::enable()
{
    basebandBuffer = std::make_unique<int16_t[]>(BASEBAND_SIZE);
    demodulator.init();
    modulator.init();

    streamRunning = false;
    txPhase = TxPhase::KEYUP;
    csma_init(&csma, CSMA_PERSIST, CSMA_SLOT_TIME, CSMA_DEFER_TIMEOUT,
              (uint32_t)getTick());
    digiDesc.status = PKT_STATUS_IDLE;

    // Descriptors submitted while the mode was off
    finishTx(-ECANCELED);
    cancelRx();
}

void OpMode_APRS::disable()
{
    // A transmission in progress cannot complete once the mode is gone
    modulator.stop();
    finishTx(-ECANCELED);

    radio_disableRtx();

    if (streamRunning) {
        audioStream_terminate(basebandId);
        audioPath_release(rxAudioPath);
        streamRunning = false;
    }

    demodulator.terminate();
    modulator.terminate();
    basebandBuffer.reset();

    // Queued receive descriptors would otherwise stay submitted to a mode
    // that is gone, and their owner could never hand them to the next one.
    cancelRx();

    platform_ledOff(GREEN);
    platform_ledOff(RED);
}

void OpMode_APRS::cancelRx()
{
    struct pktDesc *pkt;
    int ret;

    do {
        ret = rxPktQueue.tryPop(pkt);
        if (ret == 0) {
            pkt->res = -ECANCELED;
            pkt->status = PKT_STATUS_ERROR;
        }
    } while (ret != -EAGAIN);
}

void OpMode_APRS::update(rtxStatus_t *const status, const bool newCfg)
{
    if (newCfg)
        configureDigi(status);

    // Main FSM logic
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

    // Led control logic
    switch (status->opStatus) {
        case RX:
            if (demodulator.DCD())
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

void OpMode_APRS::offState(rtxStatus_t *const status)
{
    radio_enableRx();
    rxAudioPath = audioPath_request(SOURCE_RTX, SINK_MCU, PRIO_RX);
    basebandId = audioStream_start(rxAudioPath, basebandBuffer.get(),
                                   BASEBAND_SIZE, APRS_SAMPLE_RATE,
                                   STREAM_INPUT | BUF_CIRC_DOUBLE);
    if (basebandId >= 0) {
        status->opStatus = RX;
        streamRunning = true;
        return;
    }

    radio_disableRtx();
    audioPath_release(rxAudioPath);

    // Without a receiver there is no carrier to sense: transmit anyway rather
    // than hold the frame forever
    serviceDigi(status);
    if (startTxIfPending(status, false))
        return;

    // Retry in 30ms, without locking up the other tasks meanwhile
    sleepFor(0, 30);
}

void OpMode_APRS::rxState(rtxStatus_t *const status)
{
    dataBlock_t baseband = inputStream_getData(basebandId);
    if (!baseband.data) {
        stopRx(status);
        return;
    }

    if (demodulator.update(baseband)) {
        // The frame is taken once: the inbox gets a copy through its
        // descriptor, the digipeater sees it whether or not one is queued
        ssize_t len = demodulator.getFrame(rxFrame, sizeof(rxFrame));
        struct pktDesc *rxPkt;

        // With no descriptor to fill, the inbox misses the frame, as in M17,
        // and the receiver keeps running
        if (rxPktQueue.tryPop(rxPkt) == 0) {
            if ((len < 0) || ((size_t)len > rxPkt->size)) {
                rxPkt->size = 0;
                rxPkt->res = (len < 0) ? len : -EMSGSIZE;
                rxPkt->status = PKT_STATUS_ERROR;
            } else {
                memcpy(rxPkt->buffer, rxFrame, (size_t)len);
                rxPkt->res = len;
                rxPkt->status = PKT_STATUS_DONE;
            }
        }

        if (len > 0)
            aprsDigi_offer(&digi, rxFrame, (size_t)len,
                           (uint32_t)(getTick() / 1000));
    }

    serviceDigi(status);
    startTxIfPending(status, demodulator.DCD());
}

void OpMode_APRS::configureDigi(const rtxStatus_t *status)
{
    struct aprsDigiConfig cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = status->aprs_digi.mode;
    cfg.delay = status->aprs_digi.delay;
    cfg.maxHops = status->aprs_digi.maxHops;
    cfg.directOnly = (status->aprs_digi.directOnly != 0);
    strncpy(cfg.me, status->aprs_digi.call, sizeof(cfg.me) - 1);
    strncpy(cfg.alias, status->aprs_digi.alias, sizeof(cfg.alias) - 1);

    // Repeating under the factory callsign is unidentified operation
    if ((cfg.me[0] == '\0') || (strcmp(cfg.me, "N0CALL") == 0))
        cfg.mode = APRS_DIGI_OFF;
    if (cfg.delay > APRS_DIGI_MAX_DELAY)
        cfg.delay = APRS_DIGI_MAX_DELAY;
    if (cfg.maxHops == 0)
        cfg.maxHops = 1;

    aprsDigi_configure(&digi, &cfg);
}

void OpMode_APRS::serviceDigi(rtxStatus_t *status)
{
    status->aprs_digiStats.heard = digi.stats.heard;
    status->aprs_digiStats.repeated = digi.stats.repeated;
    status->aprs_digiStats.stoodDown = digi.stats.stoodDown;
    status->aprs_digiStats.refused = digi.stats.refused;

    // A user's message in the slot goes first; a held frame can wait
    if ((currTxPkt.load(std::memory_order_acquire) != nullptr)
        || (digiDesc.status == PKT_STATUS_SUBMITTED))
        return;

    size_t n = aprsDigi_due(&digi, (uint32_t)(getTick() / 1000), digiFrame,
                            sizeof(digiFrame));
    if (n == 0)
        return;

    digiDesc.buffer = digiFrame;
    digiDesc.size = n;
    digiDesc.res = 0;
    digiDesc.status = PKT_STATUS_SUBMITTED;
    if (addPacketTx(&digiDesc) != 0)
        digiDesc.status = PKT_STATUS_IDLE;
}

void OpMode_APRS::stopRx(rtxStatus_t *const status)
{
    radio_disableRtx();
    audioStream_stop(basebandId);
    audioPath_release(rxAudioPath);
    streamRunning = false;
    status->opStatus = OFF;
}

bool OpMode_APRS::startTxIfPending(rtxStatus_t *const status, bool busy)
{
    struct pktDesc *pkt = currTxPkt.load(std::memory_order_acquire);
    if (pkt == nullptr) {
        csma_idle(&csma);
        return false;
    }

    // Packet TX has no PTT behind it, so pttDisable does not apply: the
    // submitter's call to addPacketTx() is the authorization. The hard RF
    // lockout is a different thing entirely and is always respected.
    if (status->txDisable) {
        finishTx(-EPERM);
        return false;
    }

    if (!csma_mayTransmit(&csma, busy, getTick()))
        return false;

    // Receiving and transmitting cannot share the baseband path, so the RX
    // stream comes down before the radio is keyed and offState() rebuilds it
    // afterwards.
    if (streamRunning) {
        audioStream_terminate(basebandId);
        audioPath_release(rxAudioPath);
        streamRunning = false;
    }

    demodulator.reset();
    txPhase = TxPhase::KEYUP;
    csma_idle(&csma);
    status->opStatus = TX;

    return true;
}

void OpMode_APRS::txState(rtxStatus_t *const status)
{
    struct pktDesc *pkt = currTxPkt.load(std::memory_order_acquire);

    // The descriptor can only disappear here if disable() cancelled it.
    if (pkt == nullptr) {
        status->opStatus = OFF;
        return;
    }

    switch (txPhase) {
        case TxPhase::KEYUP:
            radio_disableRtx();
            radio_enableTx();

            if (modulator.start() != 0) {
                radio_disableRtx();
                finishTx(-EIO);
                status->opStatus = OFF;
                return;
            }

            encoder.start(static_cast<const uint8_t *>(pkt->buffer), pkt->size,
                          TX_DELAY_FLAGS, TX_TAIL_FLAGS);
            txPhase = TxPhase::SEND;
            return;

        case TxPhase::SEND:
            if (modulator.sendBlock(encoder))
                return;

            modulator.stop();
            radio_disableRtx();
            finishTx((ssize_t)pkt->size);

            txPhase = TxPhase::KEYUP;
            status->opStatus = OFF;
            return;
    }
}

void OpMode_APRS::finishTx(ssize_t result)
{
    struct pktDesc *pkt = currTxPkt.load(std::memory_order_acquire);
    if (pkt == nullptr)
        return;

    pkt->res = result;
    pkt->status = (result < 0) ? PKT_STATUS_ERROR : PKT_STATUS_DONE;

    // Released last: the submitter polls status, so the descriptor must be
    // fully written before the slot is handed back.
    currTxPkt.store(nullptr, std::memory_order_release);
}

int OpMode_APRS::addPacketRx(struct pktDesc *packet)
{
    if (rxPktQueue.tryPush(packet) != 0)
        return -EAGAIN;

    return 0;
}

int OpMode_APRS::addPacketTx(struct pktDesc *packet)
{
    if ((packet->size == 0) || (packet->size > (APRS_PACLEN - APRS_FCS_LEN)))
        return -EINVAL;

    struct pktDesc *expected = nullptr;
    if (!currTxPkt.compare_exchange_strong(expected, packet,
                                           std::memory_order_acq_rel,
                                           std::memory_order_relaxed))
        return -EBUSY;

    return 0;
}
