/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPMODE_MODEM_H
#define OPMODE_MODEM_H

#include "core/ringbuf.hpp"
#include "rtx/FrameModem.hpp"
#include "rtx/OpMode.hpp"

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
     * Number of frames the host can queue for transmission.
     */
    static constexpr size_t TX_SLOTS = 4;

    /**
     * Number of received frames waiting for the host before the oldest ones
     * are dropped.
     */
    static constexpr size_t RX_SLOTS = 4;

private:
    void offState(rtxStatus_t *const status);

    void rxState(rtxStatus_t *const status);

    void txState(rtxStatus_t *const status);

    /**
     * Transmission ends when no frame arrived from the host for this many
     * idle ticks of TX_IDLE_TICK_MS milliseconds each.
     */
    static constexpr uint8_t TX_HANG_TICKS = 8;
    static constexpr unsigned int TX_IDLE_TICK_MS = 10;

    FrameModem &modem;          ///< Physical layer being driven.
    const enum opmode protocol; ///< Operating mode of the physical layer.
    bool startRx;               ///< RX start pending.
    bool rxActive;              ///< Baseband sampling running.
    bool txActive;              ///< Baseband output running.
    bool locked;                ///< Demodulator locked on a transmission.
    uint8_t txIdle;             ///< Ticks spent waiting for the host in TX.
    struct modemFrame frame;    ///< Scratch frame.

    NbRingBuffer<struct modemFrame, TX_SLOTS> txQueue; ///< Host to air.
    NbRingBuffer<struct modemFrame, RX_SLOTS> rxQueue; ///< Air to host.
};

#endif /* OPMODE_MODEM_H */
