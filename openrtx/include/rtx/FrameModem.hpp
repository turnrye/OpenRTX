/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef FRAMEMODEM_H
#define FRAMEMODEM_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include "rtx/modem_frame.h"
#include "rtx/rtx.h"

/**
 * Physical layer of one protocol, seen as a frame modem: the OpMode_Modem
 * class drives it with raw frames coming from and going to a host computer
 * and knows nothing about the protocol itself. One implementation exists per
 * protocol that can be run in host-driven mode.
 *
 * All the functions are called from the rtx thread.
 */
class FrameModem
{
public:
    virtual ~FrameModem()
    {
    }

    /**
     * Allocate the resources needed by the modem.
     */
    virtual void init() = 0;

    /**
     * Release the resources allocated by init().
     */
    virtual void terminate() = 0;

    /**
     * Start receiving: baseband sampling begins, the radio stage is handled
     * by the caller.
     */
    virtual void startRx() = 0;

    /**
     * Stop receiving.
     */
    virtual void stopRx() = 0;

    /**
     * Run one receive step. The function blocks for the time needed to
     * acquire new baseband samples.
     *
     * @param frame: destination for a received frame or a marker.
     * @return true if frame has been filled.
     */
    virtual bool rxUpdate(struct modemFrame &frame) = 0;

    /**
     * Check if the demodulator is locked on a transmission.
     *
     * @return true if locked.
     */
    virtual bool rxLocked() = 0;

    /**
     * Start transmitting: baseband output begins and the preamble is sent.
     * The radio stage is handled by the caller.
     */
    virtual void startTx() = 0;

    /**
     * Transmit one frame. The function blocks for the time needed to send
     * the previous frame.
     *
     * @param frame: frame to transmit.
     */
    virtual void txFrame(const struct modemFrame &frame) = 0;

    /**
     * Stop transmitting.
     */
    virtual void stopTx() = 0;

    /**
     * Update the rtx status with what a frame passing through the modem
     * carries, for display purposes only. The protocol decides what is
     * worth showing.
     *
     * @param frame: frame being transmitted or received.
     * @param status: rtx status to update.
     */
    virtual void inspect(const struct modemFrame &frame,
                         rtxStatus_t *const status) = 0;

    /**
     * Clear the rtx status fields written by inspect().
     *
     * @param status: rtx status to update.
     */
    virtual void clearStatus(rtxStatus_t *const status) = 0;
};

#endif /* FRAMEMODEM_H */
