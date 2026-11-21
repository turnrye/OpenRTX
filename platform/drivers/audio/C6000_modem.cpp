/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <algorithm>
#include <errno.h>
#include "C6000_fmcopy.h"
#include "C6000_symbols.h"
#include "C6000_modem.h"

static constexpr uint32_t SYMBOL_RATE = 4800;
static constexpr uint32_t CODEC_RATE = 8000;
static constexpr size_t CODEC_BUF_SIZE = 320;

static const struct audioDriver *outDriver;
static const struct audioDriver *inDriver;
static const struct audioDriver *micDriver;
static bool useChip;

/*
 * The codec's microphone stream is 8 kHz at full scale, while the stream is
 * asked for at the rate of the analog path and its user applies the gain of
 * that path: each codec sample is repeated to make up the rate and divided by
 * that gain.
 */
static stream_sample_t codecBuf[CODEC_BUF_SIZE];
static struct streamCtx codecCtx;
static bool micChip;
static size_t micRepeat;
static int16_t micGain;

// TEST ONLY: microphone starts, the last start's result, and the codec
// samples since the last read: count, peak, sum and sum of squares
static uint32_t micStarts;
static int micStartRet;
static uint32_t micFrames;
static int32_t micPeak;
static int64_t micSum;
static uint64_t micSumSq;

extern "C" void C6000modem_micBench(int32_t *v, int64_t *sum, uint64_t *sumSq)
{
    v[0] = micStarts;
    v[1] = micStartRet;
    v[2] = micFrames;
    v[3] = micPeak;
    v[4] = codecCtx.running;
    *sum = micSum;
    *sumSq = micSumSq;
    micFrames = 0;
    micPeak = 0;
    micSum = 0;
    micSumSq = 0;
}

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

    micChip = useChip;
    if (micChip == false) {
        micDriver = paths->analog.driver;
        return micDriver->start(instance, paths->analog.config, ctx);
    }

    micStarts++;                        // TEST ONLY
    micStartRet = -EINVAL;              // TEST ONLY
    micRepeat = ctx->sampleRate / CODEC_RATE;
    if ((micRepeat == 0) || ((ctx->sampleRate % CODEC_RATE) != 0)
        || ((ctx->bufSize % micRepeat) != 0)
        || ((ctx->bufSize / micRepeat) > CODEC_BUF_SIZE))
        return -EINVAL;

    micDriver = paths->chip.driver;
    micGain = paths->analogGain;
    codecCtx.buffer = codecBuf;
    codecCtx.bufSize = ctx->bufSize / micRepeat;
    codecCtx.bufMode = ctx->bufMode;
    codecCtx.sampleRate = CODEC_RATE;

    int ret = micDriver->start(instance, paths->chip.config, &codecCtx);
    ctx->running = codecCtx.running;
    micStartRet = ret;                  // TEST ONLY

    return ret;
}

static int mic_data(struct streamCtx *ctx, stream_sample_t **buf)
{
    if (micChip == false)
        return micDriver->data(ctx, buf);

    stream_sample_t *in;
    int frames = micDriver->data(&codecCtx, &in);
    if (frames < 0)
        return frames;

    stream_sample_t *out = &ctx->buffer[(in - codecBuf) * micRepeat];
    for (int i = 0; i < frames; i++) {
        std::fill_n(&out[i * micRepeat], micRepeat, in[i] / micGain);

        // TEST ONLY
        micFrames++;
        micPeak = std::max<int32_t>(micPeak, (in[i] < 0) ? -in[i] : in[i]);
        micSum += in[i];
        micSumSq += static_cast<int32_t>(in[i]) * in[i];
    }

    *buf = out;
    return frames * micRepeat;
}

static int mic_sync(struct streamCtx *ctx, uint8_t dirty)
{
    if (micChip == false)
        return micDriver->sync(ctx, dirty);

    int ret = micDriver->sync(&codecCtx, dirty);
    ctx->running = codecCtx.running;

    return ret;
}

static void mic_stop(struct streamCtx *ctx)
{
    micDriver->stop(micChip ? &codecCtx : ctx);
}

static void mic_terminate(struct streamCtx *ctx)
{
    if (micDriver == NULL)
        return;

    if (micChip == false) {
        micDriver->terminate(ctx);
        return;
    }

    micDriver->terminate(&codecCtx);
    ctx->running = 0;
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
