/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef M17FRAMEMODEM_H
#define M17FRAMEMODEM_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include "protocols/M17/Demodulator.hpp"
#include "protocols/M17/FrameDecoder.hpp"
#include "protocols/M17/Modulator.hpp"
#include "rtx/FrameModem.hpp"

/**
 * M17 physical layer as a host-driven frame modem. Frames are the 48-byte
 * on-air frames (sync word plus type-4 bits); the frame type values are
 * those of the m17ModemFrame enum.
 */
class M17FrameModem : public FrameModem
{
public:
    M17FrameModem();

    ~M17FrameModem();

    virtual void init() override;

    virtual void terminate() override;

    virtual void startRx() override;

    virtual void stopRx() override;

    virtual bool rxUpdate(struct modemFrame &frame) override;

    virtual bool rxLocked() override;

    virtual void startTx() override;

    virtual void txFrame(const struct modemFrame &frame) override;

    virtual void stopTx() override;

    virtual void inspect(const struct modemFrame &frame,
                         rtxStatus_t *const status) override;

    virtual void clearStatus(rtxStatus_t *const status) override;

private:
    /**
     * Retrieve the baseband phase inversion settings of the current platform.
     */
    void getPhaseInversion();

    bool locked;                  ///< Demodulator locked on a transmission.
    bool firstFrame;              ///< Next frame is the first after sync.
    bool invertTxPhase;           ///< TX signal phase inversion setting.
    bool invertRxPhase;           ///< RX signal phase inversion setting.
    M17::Modulator modulator;     ///< M17 modulator.
    M17::Demodulator demodulator; ///< M17 demodulator.
    M17::FrameDecoder decoder;    ///< Frame decoder, for status display.
};

#endif /* M17FRAMEMODEM_H */
