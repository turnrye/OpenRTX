/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPMODE_MODEM_H
#define OPMODE_MODEM_H

#include "core/spsc_queue.hpp"
#include "rtx/FrameModem.hpp"
#include "rtx/OpMode.hpp"
#include <memory>
#include <pthread.h>

/**
 * Host-driven operating mode: the radio acts as a modem for a host computer
 * connected over a serial link. Frames pushed by the host are transmitted as
 * they are, demodulated frames are queued for the host to collect. The data
 * link layer, including who is allowed to be heard, is the host's business:
 * every frame received is forwarded.
 *
 * The class is protocol-agnostic; the protocol is provided by the FrameModem
 * instance passed at construction. The local PTT is ignored in this mode.
 */
class OpMode_Modem : public OpMode
{
public:
    /**
     * Constructor.
     *
     * @param modem: physical layer to drive.
     * @param protocol: operating mode the modem belongs to.
     */
    OpMode_Modem(FrameModem &modem, const enum opmode protocol);

    /**
     * Destructor.
     */
    ~OpMode_Modem();

    virtual void enable() override;

    virtual void disable() override;

    virtual void update(rtxStatus_t *const status, const bool newCfg) override;

    virtual opmode getID() override
    {
        return protocol;
    }

    virtual bool isHostDriven() override
    {
        return true;
    }

    virtual bool rxSquelchOpen() override
    {
        return locked;
    }

    virtual int modemPushTxFrame(const struct modemFrame *frame) override;

    virtual int modemPopRxFrame(struct modemFrame *frame) override;

    virtual size_t modemTxSpace() override;

    /**
     * Number of frames the host can queue for transmission. Hosts such as
     * m17-gateway hand over network audio as it arrives, with no jitter
     * buffer of their own, so this queue is what absorbs the jitter: 16
     * frames is 640 ms of M17 audio.
     */
    static constexpr size_t TX_SLOTS = 16;

    /**
     * Frames to accumulate before the carrier goes up, so that the first
     * network hiccup does not empty the queue at once. A lone frame still
     * goes out once it has waited TX_START_WAIT_MS.
     */
    static constexpr size_t TX_START_FRAMES = 3;
    static constexpr uint32_t TX_START_WAIT_MS = 120;

    /**
     * How long the carrier is held once the host stops sending: TX_HANG_MS
     * when it is just late, enough to ride out a network stall, and
     * TX_TAIL_MS after a frame the protocol marks as ending the
     * transmission, in case another one follows straight away. The gap is
     * filled by the physical layer's txIdle(), or waited out
     * TX_IDLE_TICK_MS at a time when it has no filler. A hard TX lockout
     * ends the transmission at once and drops what the host has queued.
     */
    static constexpr uint32_t TX_HANG_MS = 1000;
    static constexpr uint32_t TX_TAIL_MS = 80;
    static constexpr uint32_t TX_IDLE_TICK_MS = 10;

    /**
     * Number of received frames waiting for the host before new ones are
     * dropped.
     */
    static constexpr size_t RX_SLOTS = 4;

protected:
    /**
     * Get the current time. Unit tests override this and sleepMs() to run on
     * a simulated clock.
     *
     * @return current time, in milliseconds.
     */
    virtual uint32_t nowMs();

    /**
     * Pause the calling thread.
     *
     * @param ms: time to wait, in milliseconds.
     */
    virtual void sleepMs(const uint32_t ms);

private:
    void offState(rtxStatus_t *const status);

    void rxState(rtxStatus_t *const status);

    void txState(rtxStatus_t *const status);

    /**
     * Tell whether queued host frames warrant keying up. Called by the rtx
     * thread only, which notes when it first saw the queue non-empty.
     *
     * @return true if the modem should start transmitting.
     */
    bool txReady();

    /**
     * Keep the carrier up while the host has nothing to send.
     *
     * @return true once the hang time is over and the modem should key down.
     */
    bool holdCarrier();

    /**
     * The frame queues, allocated only while the mode is enabled. The rtx
     * thread owns them; the host side reaches them under queuesMutex.
     */
    struct Queues {
        SpscQueue<struct modemFrame, TX_SLOTS> tx; ///< Host to air.
        SpscQueue<struct modemFrame, RX_SLOTS> rx; ///< Air to host.
    };

    FrameModem &modem;              ///< Physical layer being driven.
    const enum opmode protocol;     ///< Operating mode of the physical layer.
    bool startRx;                   ///< RX start pending.
    bool rxActive;                  ///< Baseband sampling running.
    bool txActive;                  ///< Baseband output running.
    bool locked;                    ///< Demodulator locked on a transmission.
    bool txPending;                 ///< TX queue seen non-empty, not keyed yet.
    bool txEnding;                  ///< Last frame sent ended the transmission.
    uint32_t txPendingTick;         ///< When the TX queue was seen non-empty.
    uint32_t lastFrameTick;         ///< When the last frame went out.
    struct modemFrame frame;        ///< Scratch frame.

    std::unique_ptr<Queues> queues; ///< Frame queues, while enabled.
    pthread_mutex_t queuesMutex;    ///< Guards queues for the host side.
};

#endif /* OPMODE_MODEM_H */
