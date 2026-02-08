/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include "file_sink.h"

struct fileSinkPriv {
    FILE *fp;
    size_t halfBuf; /* Half-buffer size in samples (double-buffer mode) */
    uint8_t phase;  /* Which half of the double buffer was last written */
};

static int fileSink_start(const uint8_t instance, const void *config,
                          struct streamCtx *ctx)
{
    (void)instance;

    if (ctx == NULL)
        return -EINVAL;

    if (ctx->running != 0)
        return -EBUSY;

    struct fileSinkPriv *priv = calloc(1, sizeof(struct fileSinkPriv));
    if (priv == NULL)
        return -ENOMEM;

    priv->fp = fopen(config, "ab");
    if (priv->fp == NULL) {
        free(priv);
        return -EINVAL;
    }

    priv->halfBuf = ctx->bufSize / 2;
    priv->phase = 0;

    ctx->priv = priv;
    ctx->running = 1;

    return 0;
}

static int fileSink_data(struct streamCtx *ctx, stream_sample_t **buf)
{
    if (ctx->running == 0)
        return -1;

    struct fileSinkPriv *priv = (struct fileSinkPriv *)ctx->priv;

    if (ctx->bufMode == BUF_CIRC_DOUBLE) {
        /* Return the half that is NOT being written to disk (the idle half) */
        if (priv->phase == 0)
            *buf = ctx->buffer + priv->halfBuf;
        else
            *buf = ctx->buffer;

        return (int)priv->halfBuf;
    }

    /* Linear mode: return start of buffer */
    *buf = ctx->buffer;
    return (int)ctx->bufSize;
}

static int fileSink_sync(struct streamCtx *ctx, uint8_t dirty)
{
    if (ctx->running == 0)
        return -1;

    struct fileSinkPriv *priv = (struct fileSinkPriv *)ctx->priv;

    if (ctx->bufMode == BUF_CIRC_DOUBLE) {
        if (dirty) {
            /* Write the dirty half-buffer (the one data() last returned) */
            stream_sample_t *src;
            if (priv->phase == 0)
                src = ctx->buffer + priv->halfBuf;
            else
                src = ctx->buffer;

            fwrite(src, sizeof(stream_sample_t), priv->halfBuf, priv->fp);
            fflush(priv->fp);

            /* Toggle phase so the next data() call returns the other half */
            priv->phase ^= 1;
        }

        /* Simulate the time a real audio device would take */
        uint32_t waitTime = (1000000 * priv->halfBuf) / ctx->sampleRate;
        usleep(waitTime);
    } else {
        if (dirty) {
            fwrite(ctx->buffer, sizeof(stream_sample_t), ctx->bufSize,
                   priv->fp);
            fflush(priv->fp);
        }

        uint32_t waitTime = (1000000 * ctx->bufSize) / ctx->sampleRate;
        usleep(waitTime);
    }

    return 0;
}

static void fileSink_close(struct streamCtx *ctx)
{
    struct fileSinkPriv *priv = (struct fileSinkPriv *)ctx->priv;
    if (priv != NULL) {
        if (priv->fp != NULL)
            fclose(priv->fp);
        free(priv);
    }
    ctx->priv = NULL;
    ctx->running = 0;
}

static void fileSink_stop(struct streamCtx *ctx)
{
    if (ctx->running == 0)
        return;

    fileSink_close(ctx);
}

static void fileSink_terminate(struct streamCtx *ctx)
{
    if (ctx->running == 0)
        return;

    fileSink_close(ctx);
}

#pragma GCC diagnostic ignored "-Wpedantic"
const struct audioDriver file_sink_audio_driver = { .start = fileSink_start,
                                                    .data = fileSink_data,
                                                    .sync = fileSink_sync,
                                                    .stop = fileSink_stop,
                                                    .terminate =
                                                        fileSink_terminate };
#pragma GCC diagnostic pop
