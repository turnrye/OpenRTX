/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/dmr_debug.h"
#include "hwconfig.h"
#include "dmr.h"
#include "dmr_audio.h"

#if defined(CONFIG_DMR)

/*
 * The emulator has no baseband to talk to, so the receiver never reports a
 * call. This exists to let the mode be selected and its user interface be
 * exercised on the host.
 */

static struct dmrConfig dmrCfg;

void dmr_init(const struct dmrConfig *cfg)
{
    dmrCfg = *cfg;
    dmrDebug_log(DMR_DBG_INFO, "dmr: emulated receiver, color code %u",
                 dmrCfg.colorCode & 0x0F);
}

void dmr_terminate()
{
    dmrDebug_log(DMR_DBG_INFO, "dmr: receiver stopped");
}

void dmr_setConfig(const struct dmrConfig *cfg)
{
    dmrCfg = *cfg;
}

bool dmr_rxPoll(struct dmrBurst *burst)
{
    (void) burst;
    return false;
}

void dmr_startTx(const bool groupCall, const uint32_t dstId,
                 const uint32_t srcId)
{
    (void) groupCall;
    (void) dstId;
    (void) srcId;

    dmrDebug_log(DMR_DBG_INFO, "dmr: emulated transmitter has no baseband");
}

void dmr_stopTx()
{
}

void dmr_txTask()
{
}

bool dmr_txActive()
{
    return false;
}

void dmr_txTimeslotIsr()
{
}

/*
 * The audio pipeline of the emulated modem: there is no vocoder and no
 * radio audio path on the host, so playback never starts and no frame is
 * ever taken. The operating mode calls these exactly as it does on
 * hardware and the calls fall through, the same way an unprovisioned radio
 * degrades to header-only reception.
 */

bool dmrAudio_startRx(void)
{
    return false;
}

void dmrAudio_stopRx(void)
{
}

bool dmrAudio_running(void)
{
    return false;
}

void dmrAudio_pushFrame(const uint8_t frame[DMR_CODEC_FRAME_BYTES])
{
    (void) frame;
}

#endif /* CONFIG_DMR */
