/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rtx/M17FrameModem.hpp"
#include "interfaces/platform.h"
#include "protocols/M17/Callsign.hpp"
#include "protocols/M17/m17.h"
#include <algorithm>
#include <cstring>

#ifdef PLATFORM_MOD17
#include "calibration/calibInfo_Mod17.h"

extern mod17Calib_t mod17CalData;
#endif

using namespace M17;

M17FrameModem::M17FrameModem() :
    locked(false), firstFrame(false), invertTxPhase(false), invertRxPhase(false)
{
}

M17FrameModem::~M17FrameModem()
{
}

void M17FrameModem::init()
{
    modulator.init();
    demodulator.init();
    decoder.reset();
    locked = false;
}

void M17FrameModem::terminate()
{
    modulator.terminate();
    demodulator.terminate();
}

void M17FrameModem::startRx()
{
    getPhaseInversion();
    demodulator.startBasebandSampling();
    decoder.reset();
    locked = false;
    firstFrame = false;
}

void M17FrameModem::stopRx()
{
    demodulator.stopBasebandSampling();
    locked = false;
}

bool M17FrameModem::rxUpdate(struct modemFrame &frame)
{
    bool newData = demodulator.update(invertRxPhase);
    bool lock = demodulator.isLocked();

    // Lock lost: tell the host with a marker frame.
    if (!lock && locked) {
        locked = false;
        frame.type = M17_MODEM_LOST;
        frame.flags = 0;
        frame.len = 0;
        return true;
    }

    // Lock may be acquired a few ticks before the first frame completes:
    // remember it, so that the first frame is flagged as such.
    if (lock && !locked) {
        decoder.reset();
        firstFrame = true;
    }

    locked = lock;

    if (!lock || !newData)
        return false;

    const frame_t &data = demodulator.getFrame();

    // Classify the frame by its sync word and, as a side effect, keep the
    // link setup data up to date for the display.
    switch (decoder.decodeFrame(data)) {
        case FrameType::LINK_SETUP:
            frame.type = M17_MODEM_LSF;
            break;

        case FrameType::STREAM:
            frame.type = M17_MODEM_STREAM;
            break;

        case FrameType::PACKET:
            frame.type = M17_MODEM_PACKET;
            break;

        case FrameType::EOT:
            frame.type = M17_MODEM_EOT;
            break;

        default:
            // Preamble or garbage: the host has no use for it.
            return false;
    }

    frame.flags = firstFrame ? MODEM_FRAME_FIRST : 0;
    frame.len = data.size();
    std::copy(data.begin(), data.end(), frame.data);
    firstFrame = false;

    return true;
}

bool M17FrameModem::rxLocked()
{
    return locked;
}

void M17FrameModem::startTx()
{
    getPhaseInversion();
    decoder.reset();
    modulator.invertPhase(invertTxPhase);
    modulator.start();
    modulator.sendPreamble();
}

void M17FrameModem::txFrame(const struct modemFrame &frame)
{
    frame_t data;

    if (frame.len != data.size())
        return;

    std::copy_n(frame.data, data.size(), data.begin());

    // Decode what goes out as well, so that the display follows the relayed
    // transmission.
    if ((frame.type == M17_MODEM_LSF) || (frame.type == M17_MODEM_STREAM))
        decoder.decodeFrame(data);

    modulator.sendFrame(data);
}

void M17FrameModem::stopTx()
{
    modulator.stop();
}

void M17FrameModem::inspect(const struct modemFrame &frame,
                            rtxStatus_t *const status)
{
    // The decoder has already seen the frame in rxUpdate() or txFrame();
    // only publish the result when there is link setup data worth showing.
    if ((frame.type != M17_MODEM_LSF) && (frame.type != M17_MODEM_STREAM))
        return;

    LinkSetupFrame lsf = decoder.getLsf();
    if (!lsf.valid())
        return;

    Callsign dst = lsf.getDestination();
    Callsign src = lsf.getSource();

    status->lsfOk = true;
    strncpy(status->M17_dst, dst, sizeof(status->M17_dst));
    strncpy(status->M17_src, src, sizeof(status->M17_src));
}

void M17FrameModem::clearStatus(rtxStatus_t *const status)
{
    status->lsfOk = false;
    status->M17_src[0] = '\0';
    status->M17_dst[0] = '\0';
    status->M17_link[0] = '\0';
    status->M17_refl[0] = '\0';
    status->M17_meta_text[0] = '\0';
    decoder.reset();
}

void M17FrameModem::getPhaseInversion()
{
#if defined(PLATFORM_MD3x0) || defined(PLATFORM_MDUV3x0)
    // Invert TX phase for all MDx models, RX phase on VHF radios only.
    const hwInfo_t *hwinfo = platform_getHwInfo();
    invertTxPhase = true;
    invertRxPhase = (hwinfo->vhf_band == 1);
#elif defined(PLATFORM_MOD17)
    invertTxPhase = (mod17CalData.bb_tx_invert == 1);
    invertRxPhase = (mod17CalData.bb_rx_invert == 1);
#elif defined(PLATFORM_CS7000) || defined(PLATFORM_CS7000P)
    invertTxPhase = true;
    invertRxPhase = false;
#elif defined(PLATFORM_DM1701)
    invertTxPhase = true;
    invertRxPhase = true;
#else
    invertTxPhase = false;
    invertRxPhase = false;
#endif
}
