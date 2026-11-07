/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPMODE_APRS_H
#define OPMODE_APRS_H

#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "core/csma.h"
#include "core/ringbuf.hpp"
#include "core/dsp.h"
#include "protocols/APRS/constants.h"
#include "protocols/APRS/Demodulator.hpp"
#include "protocols/APRS/FrameEncoder.hpp"
#include "protocols/APRS/Modulator.hpp"
#include "protocols/APRS/Slicer.hpp"
#include "OpMode.hpp"
#include <atomic>
#include <memory>

/**
 * Specialization of the OpMode class for the management of APRS operating
 * mode.
 *
 * Received frames go into descriptors submitted through addPacketRx(), and
 * frames are transmitted from descriptors submitted through addPacketTx(),
 * one at a time. There is no PTT path in a packet mode.
 */

class OpMode_APRS : public OpMode
{
public:
    /**
     * Constructor.
     */
    OpMode_APRS();

    /**
     * Destructor.
     */
    ~OpMode_APRS();

    /**
     * Enable the operating mode.
     *
     * Application must ensure this function is being called when entering the
     * new operating mode and always before the first call of "update".
     */
    virtual void enable() override;

    /**
     * Disable the operating mode. This function ensures that, after being
     * called, the radio, the audio amplifier and the microphone are in OFF state.
     *
     * Application must ensure this function is being called when exiting the
     * current operating mode.
     */
    virtual void disable() override;

    /**
     * Update the internal FSM.
     * Application code has to call this function periodically, to ensure proper
     * functionality.
     *
     * @param status: pointer to the rtxStatus_t structure containing the current
     * RTX status. Internal FSM may change the current value of the opStatus flag.
     * @param newCfg: flag used inform the internal FSM that a new RTX configuration
     * has been applied.
     */
    virtual void update(rtxStatus_t *const status, const bool newCfg) override;

    /**
     * Get the mode identifier corresponding to the OpMode class.
     *
     * @return the corresponding flag from the opmode enum.
     */
    virtual opmode getID() override
    {
        return OPMODE_APRS;
    }

    /**
     * The squelch is open while the demodulator detects a carrier.
     *
     * @return true if an APRS signal is being received.
     */
    virtual bool rxSquelchOpen() override
    {
        return demodulator.DCD();
    }

    /**
     * Submit a packet reception request.
     *
     * @param packet: pointer to packet descriptor.
     * @return zero on success a negative error code otherwise.
     */
    virtual int addPacketRx(struct pktDesc *packet) override;

    /**
     * Submit a packet transmission request.
     *
     * The descriptor's buffer must hold a complete AX.25 frame without its
     * frame check sequence, and size must be its length. A single slot is
     * claimed atomically, so a second submission while one is in flight is
     * refused rather than queued.
     *
     * @param packet: pointer to packet descriptor.
     * @return zero on success, -EINVAL if the frame is empty or longer than
     * an AX.25 frame without its FCS, -EBUSY when a transmission is already
     * pending.
     */
    virtual int addPacketTx(struct pktDesc *packet) override;

private:
    /**
     * Function handling the OFF operating state.
     *
     * @param status: pointer to the rtxStatus_t structure containing
     * the current RTX status.
     */
    void offState(rtxStatus_t *const status);

    /**
     * Function handling the RX operating state.
     */
    void rxState(rtxStatus_t *const status);

    /**
     * Function handling the TX operating state.
     */
    void txState(rtxStatus_t *const status);

    /**
     * Close the baseband input stream and switch to OFF state.
     *
     * @param status: pointer to RTX status data structure.
     */
    void stopRx(rtxStatus_t *const status);

    /**
     * Hand every queued receive descriptor back, cancelled, so that its
     * owner can submit it to the next operating mode.
     */
    void cancelRx();

    /**
     * Start a pending transmission if there is one and channel access allows
     * it.
     *
     * @param status: pointer to RTX status data structure.
     * @param busy: true if a carrier is detected on the channel.
     * @return true if the operating status was switched to TX.
     */
    bool startTxIfPending(rtxStatus_t *const status, bool busy);

    /**
     * Complete the pending transmission and release its descriptor.
     *
     * @param result: value for res; negative marks the transmission failed.
     */
    void finishTx(ssize_t result);

    /**
     * Phases of one transmission. Each update() call sends one block of
     * baseband, so no call blocks for a frame's airtime.
     */
    enum class TxPhase {
        KEYUP, ///< Key the radio and start the encoder.
        SEND,  ///< Send the next block, and unkey after the last one.
    };

    /** Baseband samples in the double buffer the input stream fills. */
    static constexpr size_t BASEBAND_SIZE = 2 * APRS_BUF_SIZE;

    // Opening flags: 300 ms (TXDelay), for the receiver's squelch and clock
    // recovery to settle. Closing flags: about 30 ms.
    static constexpr size_t TX_DELAY_FLAGS = 45;
    static constexpr size_t TX_TAIL_FLAGS = 4;

    // Channel access as Dire Wolf and TNC-2 default to: p 63/256 per 100 ms
    // slot, transmitting anyway after 5 s.
    static constexpr uint8_t CSMA_PERSIST = 63;
    static constexpr uint32_t CSMA_SLOT_TIME = 100;
    static constexpr uint32_t CSMA_DEFER_TIMEOUT = 5000;

    bool streamRunning;                        ///< Baseband stream is open.
    pathId rxAudioPath;                        ///< Audio path ID for RX
    std::unique_ptr<int16_t[]> basebandBuffer; ///< buffer for RX audio handling
    streamId basebandId;                       ///< Stream ID for RX to MCU
    APRS::Demodulator demodulator; ///< Demodulator for incoming samples
    APRS::FrameEncoder encoder;    ///< HDLC framing of outgoing frames
    APRS::Modulator modulator;     ///< Modulator for outgoing frames
    NbRingBuffer<struct pktDesc *, 2> rxPktQueue; ///< Pending RX descriptors

    std::atomic<struct pktDesc *> currTxPkt;      ///< Pending TX descriptor.
    TxPhase txPhase;       ///< Phase of the transmission in progress.
    struct csmaState csma; ///< p-persistent channel access state.
};

#endif /* OPMODE_APRS_H */
