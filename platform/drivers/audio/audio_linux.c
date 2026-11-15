/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/audio.h"
#include "hwconfig.h"
#include "file_source.h"
#include "audio_sdl.h"

/*
 * Baseband input for the receive path. Raw signed 16 bit little endian samples
 * at the RX baseband rate, replayed in a loop.
 */
static const struct fileSourceConfig rxBasebandCfg = {
    .envVar = "OPENRTX_RX_BASEBAND",
    .defaultPath = "/tmp/baseband.raw",
    .loop = true,
};

/*
 * Microphone input. When OPENRTX_MIC_FILE is set the microphone reads raw
 * signed 16 bit little endian mono samples from that file, at the rate the
 * stream is opened with (for the M17 encoder: 8 kHz times
 * CONFIG_MIC_OVERSAMPLE), followed by silence once the file ends. Otherwise the
 * host's default capture device is used through SDL.
 */
static const struct fileSourceConfig micFileCfg = {
    .envVar = "OPENRTX_MIC_FILE",
    .defaultPath = NULL,
    .loop = false,
};

static const struct audioDriver *micDriver = NULL;

static const struct audioDriver *selectMicDriver(void)
{
    if (fileSource_path(&micFileCfg) != NULL)
        return &file_source_audio_driver;

    return &sdl_capture_audio_driver;
}

static int linuxMic_start(const uint8_t instance, const void *config,
                          struct streamCtx *ctx)
{
    (void)config;

    const struct audioDriver *drv = selectMicDriver();
    int ret = drv->start(instance, &micFileCfg, ctx);
    if (ret == 0)
        micDriver = drv;

    return ret;
}

static int linuxMic_data(struct streamCtx *ctx, stream_sample_t **buf)
{
    if (micDriver == NULL)
        return -1;

    return micDriver->data(ctx, buf);
}

static int linuxMic_sync(struct streamCtx *ctx, uint8_t dirty)
{
    if (micDriver == NULL)
        return -1;

    return micDriver->sync(ctx, dirty);
}

static void linuxMic_stop(struct streamCtx *ctx)
{
    if (micDriver != NULL)
        micDriver->stop(ctx);

    micDriver = NULL;
}

static void linuxMic_terminate(struct streamCtx *ctx)
{
    if (micDriver != NULL)
        micDriver->terminate(ctx);

    micDriver = NULL;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
static const struct audioDriver linux_mic_audio_driver = {
    .start = linuxMic_start,
    .data = linuxMic_data,
    .sync = linuxMic_sync,
    .stop = linuxMic_stop,
    .terminate = linuxMic_terminate,
};
#pragma GCC diagnostic pop

static const uint8_t pathCompatibilityMatrix[9][9] = {
    // MIC-SPK MIC-RTX MIC-MCU RTX-SPK RTX-RTX RTX-MCU MCU-SPK MCU-RTX MCU-MCU
    { 0, 0, 0, 1, 0, 1, 1, 0, 1 }, // MIC-RTX
    { 0, 0, 0, 0, 1, 1, 0, 1, 1 }, // MIC-SPK
    { 0, 0, 0, 1, 1, 0, 1, 1, 0 }, // MIC-MCU
    { 0, 1, 1, 0, 0, 0, 0, 1, 1 }, // RTX-SPK
    { 1, 0, 1, 0, 0, 0, 1, 0, 1 }, // RTX-RTX
    { 1, 1, 0, 0, 0, 0, 1, 1, 0 }, // RTX-MCU
    { 0, 1, 1, 0, 1, 1, 0, 0, 0 }, // MCU-SPK
    { 1, 0, 1, 1, 0, 1, 0, 0, 0 }, // MCU-RTX
    { 1, 1, 0, 1, 1, 0, 0, 0, 0 }  // MCU-MCU
};

const struct audioDevice outputDevices[] = {
    { NULL, 0, 0, SINK_MCU },
    { NULL, 0, 0, SINK_RTX },
    { &sdl_play_audio_driver, NULL, 0, SINK_SPK },
};

const struct audioDevice inputDevices[] = {
    { NULL, 0, 0, SOURCE_MCU },
    { &file_source_audio_driver, &rxBasebandCfg, 0, SOURCE_RTX },
    { &linux_mic_audio_driver, NULL, 0, SOURCE_MIC },
};

void audio_init()
{
}

void audio_terminate()
{
}

void audio_connect(const enum AudioSource source, const enum AudioSink sink)
{
#ifdef VERBOSE
    printf("Connect ");
    switch (source) {
        case SOURCE_MIC:
            printf("MIC");
            break;
        case SOURCE_RTX:
            printf("RTX");
            break;
        case SOURCE_MCU:
            printf("MCU");
            break;
        default:
            break;
    }

    printf(" with ");
    switch (sink) {
        case SINK_SPK:
            printf("SPK\n");
            break;
        case SINK_RTX:
            printf("RTX\n");
            break;
        case SINK_MCU:
            printf("MCU\n");
            break;
        default:
            break;
    }
#else
    (void)source;
    (void)sink;
#endif
}

void audio_disconnect(const enum AudioSource source, const enum AudioSink sink)
{
#ifdef VERBOSE
    printf("Disconnect ");
    switch (source) {
        case SOURCE_MIC:
            printf("MIC");
            break;
        case SOURCE_RTX:
            printf("RTX");
            break;
        case SOURCE_MCU:
            printf("MCU");
            break;
        default:
            break;
    }

    printf(" from ");
    switch (sink) {
        case SINK_SPK:
            printf("SPK\n");
            break;
        case SINK_RTX:
            printf("RTX\n");
            break;
        case SINK_MCU:
            printf("MCU\n");
            break;
        default:
            break;
    }
#else
    (void)source;
    (void)sink;
#endif
}

bool audio_checkPathCompatibility(const enum AudioSource p1Source,
                                  const enum AudioSink p1Sink,
                                  const enum AudioSource p2Source,
                                  const enum AudioSink p2Sink)

{
    uint8_t p1Index = (p1Source * 3) + p1Sink;
    uint8_t p2Index = (p2Source * 3) + p2Sink;

    return pathCompatibilityMatrix[p1Index][p2Index] == 1;
}
