/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <stdio.h>
#include <time.h>
#include "file_sink.h"

struct sinkState {
    FILE *fp;                ///< Output file.
    struct timespec playEnd; ///< When the last half buffer finishes playing.
};

static struct sinkState sink;

static uint64_t toNs(const struct timespec *ts)
{
    return ((uint64_t)ts->tv_sec * 1000000000ULL) + (uint64_t)ts->tv_nsec;
}

static void waitPlayEnd(struct sinkState *state)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    if (toNs(&now) >= toNs(&state->playEnd))
        return;

    uint64_t delta = toNs(&state->playEnd) - toNs(&now);
    struct timespec wait = { .tv_sec = (time_t)(delta / 1000000000ULL),
                             .tv_nsec = (long)(delta % 1000000000ULL) };
    nanosleep(&wait, NULL);
}

static void play(struct sinkState *state, const struct streamCtx *ctx,
                 const stream_sample_t *buf, const size_t len)
{
    // Wait for the previous half to finish, as a DMA stream would, then
    // write the new one and note when it will have finished playing.
    waitPlayEnd(state);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    uint64_t start = toNs(&state->playEnd);
    if (toNs(&now) > start)
        start = toNs(&now);

    uint64_t end = start + ((uint64_t)len * 1000000000ULL) / ctx->sampleRate;
    state->playEnd.tv_sec = (time_t)(end / 1000000000ULL);
    state->playEnd.tv_nsec = (long)(end % 1000000000ULL);

    fwrite(buf, sizeof(stream_sample_t), len, state->fp);
    fflush(state->fp);
}

static int fileSink_start(const uint8_t instance, const void *config,
                          struct streamCtx *ctx)
{
    (void)instance;

    if (ctx == NULL)
        return -EINVAL;

    if (ctx->running != 0)
        return -EBUSY;

    sink.fp = fopen(config, "ab");
    if (sink.fp == NULL)
        return -EINVAL;

    sink.playEnd.tv_sec = 0;
    sink.playEnd.tv_nsec = 0;
    ctx->priv = &sink;
    ctx->running = 1;

    // A linear buffer goes out in one go.
    if (ctx->bufMode != BUF_CIRC_DOUBLE)
        play(&sink, ctx, ctx->buffer, ctx->bufSize);

    return 0;
}

static int fileSink_data(struct streamCtx *ctx, stream_sample_t **buf)
{
    if (ctx->running == 0)
        return -1;

    // Each half is written out as soon as it is synced, so the same half
    // can always be refilled.
    *buf = ctx->buffer;
    return ctx->bufSize / 2;
}

static int fileSink_sync(struct streamCtx *ctx, uint8_t dirty)
{
    struct sinkState *state = (struct sinkState *)ctx->priv;

    if (state == NULL)
        return -1;

    if ((ctx->running != 0) && (state->fp != NULL) && (dirty != 0)
        && (ctx->bufMode == BUF_CIRC_DOUBLE))
        play(state, ctx, ctx->buffer, ctx->bufSize / 2);
    else
        waitPlayEnd(state);

    return 0;
}

static void fileSink_stop(struct streamCtx *ctx)
{
    struct sinkState *state = (struct sinkState *)ctx->priv;

    if ((ctx->running == 0) || (state == NULL))
        return;

    // What was written still plays out: the caller syncs to wait for it.
    fclose(state->fp);
    state->fp = NULL;
    ctx->running = 0;
}

static void fileSink_terminate(struct streamCtx *ctx)
{
    struct sinkState *state = (struct sinkState *)ctx->priv;

    if ((ctx->running == 0) || (state == NULL))
        return;

    fclose(state->fp);
    state->fp = NULL;
    state->playEnd.tv_sec = 0;
    state->playEnd.tv_nsec = 0;
    ctx->running = 0;
}

const struct audioDriver file_sink_audio_driver = {
    .start = fileSink_start,
    .data = fileSink_data,
    .sync = fileSink_sync,
    .stop = fileSink_stop,
    .terminate = fileSink_terminate,
};
