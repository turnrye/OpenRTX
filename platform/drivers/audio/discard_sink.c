/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include "discard_sink.h"

static int discardSink_start(const uint8_t instance, const void *config,
                             struct streamCtx *ctx)
{
    (void)instance;
    (void)config;

    if (ctx == NULL)
        return -EINVAL;

    if (ctx->running != 0)
        return -EBUSY;

    ctx->running = 1;

    return 0;
}

static int discardSink_data(struct streamCtx *ctx, stream_sample_t **buf)
{
    if (ctx->running == 0)
        return -1;

    if (ctx->bufMode == BUF_CIRC_DOUBLE) {
        /* Always return the first half as "idle" — data is discarded anyway */
        *buf = ctx->buffer;
        return (int)(ctx->bufSize / 2);
    }

    *buf = ctx->buffer;
    return (int)ctx->bufSize;
}

static int discardSink_sync(struct streamCtx *ctx, uint8_t dirty)
{
    (void)dirty;

    size_t size = ctx->bufSize;
    if (ctx->bufMode == BUF_CIRC_DOUBLE)
        size /= 2;

    /* Simulate the time a real audio device would take to consume this data */
    uint32_t waitTime = (1000000 * size) / ctx->sampleRate;
    usleep(waitTime);

    return 0;
}

static void discardSink_stop(struct streamCtx *ctx)
{
    ctx->running = 0;
}

static void discardSink_terminate(struct streamCtx *ctx)
{
    ctx->running = 0;
}

#pragma GCC diagnostic ignored "-Wpedantic"
const struct audioDriver discard_sink_audio_driver = {
    .start = discardSink_start,
    .data = discardSink_data,
    .sync = discardSink_sync,
    .stop = discardSink_stop,
    .terminate = discardSink_terminate
};
#pragma GCC diagnostic pop
