/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include "file_source.h"

struct fileSourceState {
    FILE *fp;
    bool loop;
};

const char *fileSource_path(const struct fileSourceConfig *cfg)
{
    if (cfg == NULL)
        return NULL;

    if (cfg->envVar != NULL) {
        const char *env = getenv(cfg->envVar);
        if ((env != NULL) && (env[0] != '\0'))
            return env;
    }

    return cfg->defaultPath;
}

static int fileSource_start(const uint8_t instance, const void *config,
                            struct streamCtx *ctx)
{
    (void)instance;

    if ((ctx == NULL) || (config == NULL))
        return -EINVAL;

    if (ctx->running != 0)
        return -EBUSY;

    const struct fileSourceConfig *cfg = config;
    const char *path = fileSource_path(cfg);
    if (path == NULL)
        return -EINVAL;

    struct fileSourceState *state = malloc(sizeof(struct fileSourceState));
    if (state == NULL)
        return -ENOMEM;

    state->fp = fopen(path, "rb");
    if (state->fp == NULL) {
        free(state);
        return -EINVAL;
    }

    state->loop = cfg->loop;
    ctx->priv = state;
    ctx->running = 1;

    return 0;
}

static int fileSource_data(struct streamCtx *ctx, stream_sample_t **buf)
{
    if (ctx->running == 0)
        return -1;

    struct fileSourceState *state = (struct fileSourceState *)ctx->priv;
    stream_sample_t *dest = ctx->buffer;
    size_t size = ctx->bufSize;
    size_t i = 0;

    if (ctx->bufMode == BUF_CIRC_DOUBLE)
        size /= 2;

    while (i < size) {
        size_t n = fread(dest + i, sizeof(stream_sample_t), size - i, state->fp);
        i += n;

        if (i < size) {
            if (state->loop == false) {
                // End of file: pad the rest of the block with silence
                memset(dest + i, 0, (size - i) * sizeof(stream_sample_t));
                break;
            }

            // Rollover to the beginning of the file. Stop if the file is
            // empty, to avoid spinning forever.
            if ((n == 0) && (ftell(state->fp) == 0)) {
                memset(dest + i, 0, (size - i) * sizeof(stream_sample_t));
                break;
            }

            fseek(state->fp, 0, SEEK_SET);
        }
    }

    *buf = dest;
    return size;
}

static int fileSource_sync(struct streamCtx *ctx, uint8_t dirty)
{
    (void)dirty;

    size_t size = ctx->bufSize;
    if (ctx->bufMode == BUF_CIRC_DOUBLE)
        size /= 2;

    // Simulate the time needed to get a new chunck of data from an equivalent
    // hardware peripheral.
    uint32_t waitTime = (1000000 * size) / ctx->sampleRate;
    usleep(waitTime);

    return 0;
}

static void fileSource_close(struct streamCtx *ctx)
{
    if (ctx->running == 0)
        return;

    struct fileSourceState *state = (struct fileSourceState *)ctx->priv;
    fclose(state->fp);
    free(state);

    // Allow the stream to be started again
    ctx->priv = NULL;
    ctx->running = 0;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
const struct audioDriver file_source_audio_driver = {
    .start = fileSource_start,
    .data = fileSource_data,
    .sync = fileSource_sync,
    .stop = fileSource_close,
    .terminate = fileSource_close,
};
#pragma GCC diagnostic pop
