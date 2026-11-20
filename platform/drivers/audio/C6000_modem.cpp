/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "C6000_fmcopy.h"
#include "C6000_symbols.h"
#include "C6000_modem.h"

static constexpr uint32_t SYMBOL_RATE = 4800;

static const struct audioDriver *outDriver;
static const struct audioDriver *inDriver;
static const struct audioDriver *micDriver;
static bool useChip;

extern "C" void C6000modem_bench(uint32_t *v) // TEST ONLY
{
    extern const struct audioDriver C6000_symbols_audio_driver;
    v[0] = (outDriver == &C6000_symbols_audio_driver) ? 2
           : (outDriver != NULL) ? 1 : 0;
    v[1] = useChip ? 1 : 0;
}

void C6000modem_useChip(const bool chip)
{
    useChip = chip;
}

static int out_start(const uint8_t instance, const void *config,
                     struct streamCtx *ctx)
{
    const struct C6000modemPath *analog =
        static_cast<const struct C6000modemPath *>(config);

    if (ctx->sampleRate == SYMBOL_RATE) {
        outDriver = &C6000_symbols_audio_driver;
        return outDriver->start(instance, NULL, ctx);
    }

    outDriver = analog->driver;
    return outDriver->start(instance, analog->config, ctx);
}

static int out_data(struct streamCtx *ctx, stream_sample_t **buf)
{
    return outDriver->data(ctx, buf);
}

static int out_sync(struct streamCtx *ctx, uint8_t dirty)
{
    return outDriver->sync(ctx, dirty);
}

static void out_stop(struct streamCtx *ctx)
{
    outDriver->stop(ctx);
}

static void out_terminate(struct streamCtx *ctx)
{
    if (outDriver != NULL)
        outDriver->terminate(ctx);
}

static int in_start(const uint8_t instance, const void *config,
                    struct streamCtx *ctx)
{
    const struct C6000modemPath *analog =
        static_cast<const struct C6000modemPath *>(config);

    if (useChip) {
        inDriver = &C6000_fmcopy_audio_driver;
        return inDriver->start(instance, NULL, ctx);
    }

    inDriver = analog->driver;
    return inDriver->start(instance, analog->config, ctx);
}

static int in_data(struct streamCtx *ctx, stream_sample_t **buf)
{
    return inDriver->data(ctx, buf);
}

static int in_sync(struct streamCtx *ctx, uint8_t dirty)
{
    return inDriver->sync(ctx, dirty);
}

static void in_stop(struct streamCtx *ctx)
{
    inDriver->stop(ctx);
}

static void in_terminate(struct streamCtx *ctx)
{
    if (inDriver != NULL)
        inDriver->terminate(ctx);
}

static int mic_start(const uint8_t instance, const void *config,
                     struct streamCtx *ctx)
{
    const struct C6000micPaths *paths =
        static_cast<const struct C6000micPaths *>(config);
    const struct C6000modemPath *path = useChip ? &paths->chip : &paths->analog;

    micDriver = path->driver;
    return micDriver->start(instance, path->config, ctx);
}

static int mic_data(struct streamCtx *ctx, stream_sample_t **buf)
{
    return micDriver->data(ctx, buf);
}

static int mic_sync(struct streamCtx *ctx, uint8_t dirty)
{
    return micDriver->sync(ctx, dirty);
}

static void mic_stop(struct streamCtx *ctx)
{
    micDriver->stop(ctx);
}

static void mic_terminate(struct streamCtx *ctx)
{
    if (micDriver != NULL)
        micDriver->terminate(ctx);
}

#pragma GCC diagnostic ignored "-Wpedantic"
const struct audioDriver C6000_modem_out_audio_driver = {
    .start = out_start,
    .data = out_data,
    .sync = out_sync,
    .stop = out_stop,
    .terminate = out_terminate,
};

const struct audioDriver C6000_modem_in_audio_driver = {
    .start = in_start,
    .data = in_data,
    .sync = in_sync,
    .stop = in_stop,
    .terminate = in_terminate,
};

const struct audioDriver C6000_modem_mic_audio_driver = {
    .start = mic_start,
    .data = mic_data,
    .sync = mic_sync,
    .stop = mic_stop,
    .terminate = mic_terminate,
};
#pragma GCC diagnostic pop
