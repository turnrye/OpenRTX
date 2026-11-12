/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include "file_source.h"
#include "interfaces/delays.h"
#include "emulator/emulator.h"

static const char *basePath; // File played on a frequency with none of its own
static uint32_t openFreq;    // RX frequency the open file belongs to

/**
 * Open the file for the frequency the receiver is tuned to: the base path
 * with "_<frequency in Hz>" before its extension, else the base path itself.
 * A file of a frequency plays on whether it is listened to or not, so it is
 * opened at the point given by the system time.
 *
 * @param ctx: stream context.
 * @return file handle, NULL on failure.
 */
static FILE *openForFrequency(const struct streamCtx *ctx)
{
    char path[256];
    const char *ext = strrchr(basePath, '.');
    int stem = (ext != NULL) ? (int)(ext - basePath) : (int)strlen(basePath);

    openFreq = emulator_state.rxFrequency;
    snprintf(path, sizeof(path), "%.*s_%u%s", stem, basePath,
             (unsigned)openFreq, (ext != NULL) ? ext : "");

    FILE *fp = fopen(path, "rb");
    if (fp == NULL)
        return fopen(basePath, "rb");

    fseek(fp, 0, SEEK_END);
    long samples = ftell(fp) / (long)sizeof(stream_sample_t);
    if (samples == 0) {
        fclose(fp);
        return fopen(basePath, "rb");
    }

    long pos = (long)((getTick() * ctx->sampleRate / 1000) % samples);
    fseek(fp, pos * (long)sizeof(stream_sample_t), SEEK_SET);

    return fp;
}

static int fileSource_start(const uint8_t instance, const void *config,
                            struct streamCtx *ctx)
{
    (void)instance;

    if (ctx == NULL)
        return -EINVAL;

    if (ctx->running != 0)
        return -EBUSY;

    ctx->running = 1;
    basePath = config;

    FILE *fp = openForFrequency(ctx);
    if (fp == NULL) {
        ctx->running = 0;
        return -EINVAL;
    }

    ctx->priv = fp;

    return 0;
}

static int fileSource_data(struct streamCtx *ctx, stream_sample_t **buf)
{
    if (ctx->running == 0)
        return -1;

    FILE *fp = (FILE *)ctx->priv;

    // Follow the receiver when it is retuned with the stream open
    if (emulator_state.rxFrequency != openFreq) {
        FILE *next = openForFrequency(ctx);
        if (next != NULL) {
            fclose(fp);
            fp = next;
            ctx->priv = fp;
        }
    }

    stream_sample_t *dest = ctx->buffer;
    *buf = dest;
    size_t size = ctx->bufSize;
    size_t i = 0;

    if (ctx->bufMode == BUF_CIRC_DOUBLE)
        size /= 2;

    // Read data from the file, rollover when end is reached
    while (i < size) {
        size_t n = fread(dest + i, sizeof(stream_sample_t), size - i, fp);
        if (n < (size - i))
            fseek(fp, 0, SEEK_SET);

        i += n;
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

static void fileSource_stop(struct streamCtx *ctx)
{
    if (ctx->running == 0)
        return;

    FILE *fp = (FILE *)ctx->priv;
    fclose(fp);
    ctx->running = 0;
}

static void fileSource_halt(struct streamCtx *ctx)
{
    if (ctx->running == 0)
        return;

    FILE *fp = (FILE *)ctx->priv;
    fclose(fp);
    ctx->running = 0;
}

#pragma GCC diagnostic ignored "-Wpedantic"
const struct audioDriver file_source_audio_driver = {
    .start = fileSource_start,
    .data = fileSource_data,
    .sync = fileSource_sync,
    .stop = fileSource_stop,
    .terminate = fileSource_halt,
};
#pragma GCC diagnostic pop
