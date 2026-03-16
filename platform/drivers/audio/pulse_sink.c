/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pulse/simple.h>
#include <pulse/error.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include "pulse_sink.h"

struct pulseSinkPriv {
    pa_simple *pa;
    size_t halfBuf; /* Half-buffer size in elements (double-buffer mode) */
    uint8_t phase;  /* Which half of the double buffer was last written */
};

static int pulseSink_start(const uint8_t instance, const void *config,
                           struct streamCtx *ctx)
{
    (void)instance;
    (void)config;

    if (ctx == NULL)
        return -EINVAL;

    if (ctx->running != 0)
        return -EBUSY;

    struct pulseSinkPriv *priv = calloc(1, sizeof(struct pulseSinkPriv));
    if (priv == NULL)
        return -ENOMEM;

    priv->halfBuf = ctx->bufSize / 2;
    priv->phase = 0;

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.channels = 1;
    ss.rate = ctx->sampleRate;

    // Set buffer attributes for low-latency playback.  Without explicit
    // attributes PulseAudio uses a large default buffer which adds audible
    // delay to decoded audio.
    pa_buffer_attr ba;
    memset(&ba, 0xff, sizeof(ba)); // (uint32_t)-1 = "don't care"
    ba.tlength = priv->halfBuf * sizeof(stream_sample_t) * 2;
    ba.maxlength = (uint32_t)-1;

    int error = 0;
    priv->pa = pa_simple_new(NULL, "OpenRTX", PA_STREAM_PLAYBACK, NULL,
                             "OpenRTX", &ss, NULL, &ba, &error);
    if (priv->pa == NULL) {
        free(priv);
        return -EIO;
    }

    ctx->priv = priv;
    ctx->running = 1;

    return 0;
}

static int pulseSink_data(struct streamCtx *ctx, stream_sample_t **buf)
{
    if (ctx->running == 0)
        return -1;

    struct pulseSinkPriv *priv = (struct pulseSinkPriv *)ctx->priv;

    if (ctx->bufMode == BUF_CIRC_DOUBLE) {
        /* Return the half-buffer that is NOT being played (idle half) */
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

static int pulseSink_sync(struct streamCtx *ctx, uint8_t dirty)
{
    struct pulseSinkPriv *priv = (struct pulseSinkPriv *)ctx->priv;
    int error = 0;

    if (ctx->bufMode == BUF_CIRC_DOUBLE) {
        if (dirty) {
            /* Write the dirty half-buffer to PulseAudio */
            stream_sample_t *src;
            if (priv->phase == 0)
                src = ctx->buffer + priv->halfBuf;
            else
                src = ctx->buffer;

            int ret = pa_simple_write(
                priv->pa, src, priv->halfBuf * sizeof(stream_sample_t), &error);
            if (ret < 0)
                return -EIO;

            /* Toggle phase */
            priv->phase ^= 1;
        }
    } else {
        /* Linear mode: write entire buffer */
        if (dirty) {
            int ret = pa_simple_write(priv->pa, ctx->buffer,
                                      ctx->bufSize * sizeof(stream_sample_t),
                                      &error);
            if (ret < 0)
                return -EIO;
        }
    }

    return 0;
}

static void pulseSink_stop(struct streamCtx *ctx)
{
    if (ctx->running == 0)
        return;

    struct pulseSinkPriv *priv = (struct pulseSinkPriv *)ctx->priv;
    if (priv != NULL && priv->pa != NULL) {
        int error = 0;
        pa_simple_drain(priv->pa, &error);
        pa_simple_free(priv->pa);
        priv->pa = NULL;
    }

    free(priv);
    ctx->priv = NULL;
    ctx->running = 0;
}

static void pulseSink_terminate(struct streamCtx *ctx)
{
    if (ctx->running == 0)
        return;

    struct pulseSinkPriv *priv = (struct pulseSinkPriv *)ctx->priv;
    if (priv != NULL && priv->pa != NULL) {
        pa_simple_free(priv->pa);
        priv->pa = NULL;
    }

    free(priv);
    ctx->priv = NULL;
    ctx->running = 0;
}

#pragma GCC diagnostic ignored "-Wpedantic"
const struct audioDriver pulse_sink_audio_driver = { .start = pulseSink_start,
                                                     .data = pulseSink_data,
                                                     .sync = pulseSink_sync,
                                                     .stop = pulseSink_stop,
                                                     .terminate =
                                                         pulseSink_terminate };
#pragma GCC diagnostic pop
