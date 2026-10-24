/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/dmr_debug.h"
#include "hwconfig.h"
#include "interfaces/dmr.h"
#include "interfaces/dmr_codec.h"
#include <string.h>

#if defined(CONFIG_DMR)

/*
 * The emulator has no baseband to talk to, so the receiver never reports a
 * call. This exists to let the mode be selected and its user interface be
 * exercised on the host.
 */

static uint8_t configuredCc;

void dmr_init(const struct dmrConfig *config)
{
    configuredCc = config->colorCode & 0x0F;
    dmrDebug_log(DMR_DBG_INFO, "dmr: emulated receiver, color code %u",
                 configuredCc);
}

void dmr_terminate()
{
    dmrDebug_log(DMR_DBG_INFO, "dmr: receiver stopped");
}

void dmr_configure(const struct dmrConfig *config)
{
    configuredCc = config->colorCode & 0x0F;
}

void dmr_task(struct dmrRxState *rx)
{
    memset(rx, 0, sizeof(struct dmrRxState));
}

void dmr_startTx(const bool groupCall, const uint32_t dstId)
{
    (void)groupCall;
    (void)dstId;

    dmrDebug_log(DMR_DBG_INFO, "dmr: emulated transmitter has no baseband");
}

void dmr_stopTx()
{
}

void dmr_txTask()
{
}

enum dmrTxReadiness dmr_txReadiness()
{
    return DMR_TX_READY;
}

void dmr_startWakeup()
{
}

bool dmr_txActive()
{
    return false;
}

/*
 * Nor has it the vocoder: the voice pipeline finds it missing, as on a radio
 * whose image carries no codec, and neither plays nor captures.
 */

bool dmr_codec_available(void)
{
    return false;
}

bool dmr_codec_init(void)
{
    return false;
}

void dmr_codec_terminate(void)
{
}

void dmr_codec_encode(uint8_t frame[DMR_CODEC_FRAME_BYTES],
                      const int16_t pcm[DMR_CODEC_PCM_SAMPLES])
{
    (void)pcm;
    memset(frame, 0, DMR_CODEC_FRAME_BYTES);
}

void dmr_codec_decode(int16_t pcm[DMR_CODEC_PCM_SAMPLES],
                      const uint8_t frame[DMR_CODEC_FRAME_BYTES])
{
    (void)frame;
    memset(pcm, 0, DMR_CODEC_PCM_SAMPLES * sizeof(int16_t));
}

void dmr_codec_selfTest(void)
{
}

#endif /* CONFIG_DMR */
