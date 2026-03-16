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
#include "pulse_source.h"

struct pulseSourcePriv {
    pa_simple *pa;
    size_t halfBuf; /* Half-buffer size in elements (double-buffer mode) */
    uint8_t phase;  /* Tracks which half was most recently filled */
};

static int pulseSource_start(const uint8_t instance, const void *config,
                             struct streamCtx *ctx)
{
    (void)instance;
    (void)config;

    if (ctx == NULL)
        return -EINVAL;

    if (ctx->running != 0)
        return -EBUSY;

    struct pulseSourcePriv *priv = calloc(1, sizeof(struct pulseSourcePriv));
    if (priv == NULL)
        return -ENOMEM;

    priv->halfBuf = ctx->bufSize / 2;
    priv->phase = 0;

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.channels = 1;
    ss.rate = ctx->sampleRate;

    // Set buffer attributes for low-latency recording.  Without explicit
    // attributes PulseAudio uses a large default buffer (~2 s) which causes
    // pa_simple_read() to deliver audio in bursts instead of smoothly.
    // fragsize is the amount of data PulseAudio delivers per read request;
    // setting it to the half-buffer size keeps reads aligned with the
    // double-buffer scheme.
    pa_buffer_attr ba;
    memset(&ba, 0xff, sizeof(ba)); // (uint32_t)-1 = "don't care"
    ba.fragsize = priv->halfBuf * sizeof(stream_sample_t);

    int error = 0;
    priv->pa = pa_simple_new(NULL, "OpenRTX", PA_STREAM_RECORD, NULL,
                             "OpenRTX MIC", &ss, NULL, &ba, &error);
    if (priv->pa == NULL) {
        free(priv);
        return -EIO;
    }

    ctx->priv = priv;
    ctx->running = 1;

    return 0;
}

static int pulseSource_data(struct streamCtx *ctx, stream_sample_t **buf)
{
    if (ctx->running == 0)
        return -1;

    struct pulseSourcePriv *priv = (struct pulseSourcePriv *)ctx->priv;

    if (ctx->bufMode == BUF_CIRC_DOUBLE) {
        /* Return the half that was most recently filled by sync() */
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

static int pulseSource_sync(struct streamCtx *ctx, uint8_t dirty)
{
    (void)dirty;

    struct pulseSourcePriv *priv = (struct pulseSourcePriv *)ctx->priv;
    int error = 0;

    if (ctx->bufMode == BUF_CIRC_DOUBLE) {
        /* Read into the current phase's half-buffer (blocking) */
        stream_sample_t *dest;
        if (priv->phase == 0)
            dest = ctx->buffer;
        else
            dest = ctx->buffer + priv->halfBuf;

        int ret = pa_simple_read(
            priv->pa, dest, priv->halfBuf * sizeof(stream_sample_t), &error);
        if (ret < 0)
            return -EIO;

        /* Toggle phase so data() returns the just-filled half */
        priv->phase ^= 1;
    } else {
        /* Linear mode: read entire buffer */
        int ret = pa_simple_read(priv->pa, ctx->buffer,
                                 ctx->bufSize * sizeof(stream_sample_t),
                                 &error);
        if (ret < 0)
            return -EIO;
    }

    return 0;
}

static void pulseSource_stop(struct streamCtx *ctx)
{
    if (ctx->running == 0)
        return;

    struct pulseSourcePriv *priv = (struct pulseSourcePriv *)ctx->priv;
    if (priv != NULL && priv->pa != NULL) {
        pa_simple_free(priv->pa);
        priv->pa = NULL;
    }

    free(priv);
    ctx->priv = NULL;
    ctx->running = 0;
}

static void pulseSource_terminate(struct streamCtx *ctx)
{
    if (ctx->running == 0)
        return;

    struct pulseSourcePriv *priv = (struct pulseSourcePriv *)ctx->priv;
    if (priv != NULL && priv->pa != NULL) {
        pa_simple_free(priv->pa);
        priv->pa = NULL;
    }

    free(priv);
    ctx->priv = NULL;
    ctx->running = 0;
}

#pragma GCC diagnostic ignored "-Wpedantic"
const struct audioDriver pulse_source_audio_driver = {
    .start = pulseSource_start,
    .data = pulseSource_data,
    .sync = pulseSource_sync,
    .stop = pulseSource_stop,
    .terminate = pulseSource_terminate
};
#pragma GCC diagnostic pop
