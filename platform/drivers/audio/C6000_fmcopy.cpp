/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <cmath>
#include <cstring>
#include <errno.h>
#include <new>
#include <pthread.h>
#include "interfaces/delays.h"
#include "C6000_fmcopy.h"

/*
 * The HR_C6000 FM receive copy (RadioFoundry, docs/spec/modem-hrc6000.md
 * 10.6): with the chip in FM mode and every receive audio filter off, its data
 * RAM at 0x030 holds two halves of 256 big endian samples at 8 kHz, and
 * system interrupt flag 0x82 bit 1 rises each time one is full. The flag does
 * not say which half: the one whose last sample changed is.
 *
 * A worker thread reads each half, interpolates it to 24 kHz and queues it.
 * The stream takes its halves from that queue in sync(), so the worker's
 * 32 ms bursts never overwrite a half being read.
 */

static constexpr uint16_t COPY_RAM = 0x030;
static constexpr size_t COPY_HALF = 512; // Bytes: 256 samples
static constexpr size_t PHASES = 3;      // 8 kHz to 24 kHz
static constexpr size_t TAPS = 12;       // Per phase
static constexpr size_t QUEUE = 4096;    // Samples at 24 kHz
static constexpr int SHIFT = 4;          // Copy at +-16k to about +-1k

static const uint8_t saveRegs[] = { 0x41, 0x36, 0x60, 0x34, 0x10, 0x81 };

static HR_C6000 *c6000;
static uint8_t saved[sizeof(saveRegs)];
static float taps[PHASES][TAPS];
static float hist[TAPS];
static int16_t *queue;
static size_t qHead;
static size_t qCount;
static uint8_t readyHalf;
static uint8_t nextHalf;
static volatile bool running;
static volatile bool stopReq;
static pthread_t worker;
static pthread_mutex_t qMutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * Windowed sinc low-pass at 3.6 kHz for the 24 kHz rate, with a gain of 3 to
 * keep the level after zero stuffing.
 */
static void makeTaps()
{
    const size_t n = PHASES * TAPS;
    const float fc = 3600.0f / 24000.0f;

    for (size_t i = 0; i < n; i++) {
        float m = (float)i - (float)(n - 1) / 2.0f;
        float s = 2.0f * fc;
        if (m != 0.0f)
            s = sinf(2.0f * (float)M_PI * fc * m) / ((float)M_PI * m);

        float w = 0.54f
                - 0.46f * cosf(2.0f * (float)M_PI * (float)i / (float)(n - 1));
        taps[i % PHASES][i / PHASES] = 3.0f * s * w;
    }
}

/**
 * Last sample of a copy half.
 *
 * @param half: 0 or 1.
 * @return the sample.
 */
static int16_t lastSample(const int half)
{
    uint8_t b[2];
    uint16_t addr = COPY_RAM + half * COPY_HALF + COPY_HALF - 2;

    c6000->readRamExt(2, addr, b, sizeof(b));
    return (int16_t)((b[0] << 8) | b[1]);
}

/**
 * Put the chip in FM mode with every receive audio filter off and the copy's
 * interrupt flag on, saving what it changes.
 */
static void copyOn()
{
    for (size_t i = 0; i < sizeof(saveRegs); i++)
        saved[i] = c6000->readCfgRegister(saveRegs[i]);

    c6000->writeCfgRegister(0x41, 0x00); // No physical layer slot activity
    c6000->writeCfgRegister(0x36, 0x00); // FM audio not to the codec
    c6000->writeCfgRegister(0x60, 0x00); // No transmission
    c6000->writeCfgRegister(0x34, 0x00); // Receive filters off
    c6000->writeCfgRegister(0x10, 0x80); // FM mode
    c6000->writeCfgRegister(0x81, saved[5] | 0x02);
    c6000->writeCfgRegister(0x83, 0xFF);
}

/**
 * Restore the registers copyOn() changed.
 */
static void copyOff()
{
    for (size_t i = sizeof(saveRegs); i > 0; i--)
        c6000->writeCfgRegister(saveRegs[i - 1], saved[i - 1]);

    c6000->writeCfgRegister(0x83, 0xFF);
}

/**
 * Queue interpolated samples, dropping the oldest when the queue is full.
 *
 * @param s: samples.
 * @param n: number of samples.
 */
static void enqueue(const int16_t *s, const size_t n)
{
    pthread_mutex_lock(&qMutex);
    for (size_t i = 0; i < n; i++) {
        if (qCount >= QUEUE) {
            qHead = (qHead + 1) % QUEUE;
            qCount--;
        }

        queue[(qHead + qCount) % QUEUE] = s[i];
        qCount++;
    }
    pthread_mutex_unlock(&qMutex);
}

/**
 * Interpolate one copy half to 24 kHz.
 *
 * @param raw: the half as read, big endian.
 * @param out: PHASES samples for each input sample.
 */
static void interpolate(const uint8_t *raw, int16_t *out)
{
    size_t o = 0;

    for (size_t i = 0; i < COPY_HALF / 2; i++) {
        int16_t v = (int16_t)((raw[2 * i] << 8) | raw[2 * i + 1]);

        memmove(&hist[1], &hist[0], (TAPS - 1) * sizeof(float));
        hist[0] = (float)(v >> SHIFT);

        for (size_t p = 0; p < PHASES; p++) {
            float acc = 0.0f;
            for (size_t k = 0; k < TAPS; k++)
                acc += taps[p][k] * hist[k];

            if (acc > 32767.0f)
                acc = 32767.0f;
            if (acc < -32768.0f)
                acc = -32768.0f;

            out[o++] = (int16_t)acc;
        }
    }
}

static void *workerFunc(void *arg)
{
    (void)arg;
    static uint8_t raw[COPY_HALF];
    static int16_t out[PHASES * COPY_HALF / 2];

    copyOn();
    int16_t last[2] = { lastSample(0), lastSample(1) };
    int lastHalf = 1;

    while (stopReq == false) {
        if ((c6000->readCfgRegister(0x82) & 0x02) == 0) {
            sleepFor(0, 2);
            continue;
        }

        c6000->writeCfgRegister(0x83, 0x02);

        // The half whose last sample changed is the full one; when both or
        // neither did (a steady input), it is the other one than last time
        const int16_t now[2] = { lastSample(0), lastSample(1) };
        const bool ch0 = (now[0] != last[0]);
        const bool ch1 = (now[1] != last[1]);
        int half = lastHalf ^ 1;
        if (ch0 && !ch1)
            half = 0;
        else if (ch1 && !ch0)
            half = 1;

        c6000->readRamExt(2, COPY_RAM + half * COPY_HALF, raw, COPY_HALF);
        last[0] = now[0];
        last[1] = now[1];
        lastHalf = half;

        interpolate(raw, out);
        enqueue(out, PHASES * COPY_HALF / 2);
    }

    copyOff();
    return NULL;
}

void C6000fmcopy_init(HR_C6000 *device)
{
    c6000 = device;
    makeTaps();
}

static int fmcopy_start(const uint8_t instance, const void *config,
                        struct streamCtx *ctx)
{
    (void)instance;
    (void)config;

    if ((ctx == NULL) || (ctx->running != 0) || running)
        return -EBUSY;

    if ((ctx->sampleRate != 24000) || (ctx->bufMode != BUF_CIRC_DOUBLE)
        || ((ctx->bufSize % 2) != 0))
        return -EINVAL;

    if (queue == NULL)
        queue = new (std::nothrow) int16_t[QUEUE];

    if ((queue == NULL) || (c6000 == NULL))
        return -ENOMEM;

    memset(hist, 0, sizeof(hist));
    qHead = 0;
    qCount = 0;
    readyHalf = 0;
    nextHalf = 0;
    stopReq = false;
    ctx->running = 1;
    running = true;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2048);
    if (pthread_create(&worker, &attr, workerFunc, NULL) != 0) {
        ctx->running = 0;
        running = false;
        return -ENOMEM;
    }

    return 0;
}

static int fmcopy_data(struct streamCtx *ctx, stream_sample_t **buf)
{
    const size_t half = ctx->bufSize / 2;

    *buf = &ctx->buffer[readyHalf * half];
    return half;
}

static int fmcopy_sync(struct streamCtx *ctx, uint8_t dirty)
{
    (void)dirty;
    const size_t half = ctx->bufSize / 2;

    // A whole half from the queue: one copy half gives 768 samples
    for (int i = 0; i < 200; i++) {
        if (running == false)
            return -1;

        pthread_mutex_lock(&qMutex);
        bool ready = (qCount >= half);
        if (ready) {
            int16_t *dst = &ctx->buffer[nextHalf * half];
            for (size_t k = 0; k < half; k++)
                dst[k] = queue[(qHead + k) % QUEUE];

            qHead = (qHead + half) % QUEUE;
            qCount -= half;
        }
        pthread_mutex_unlock(&qMutex);

        if (ready) {
            readyHalf = nextHalf;
            nextHalf ^= 1;
            return 0;
        }

        sleepFor(0, 1);
    }

    return -1;
}

static void fmcopy_halt(struct streamCtx *ctx)
{
    if (running == false)
        return;

    stopReq = true;
    pthread_join(worker, NULL);
    running = false;
    ctx->running = 0;
}

#pragma GCC diagnostic ignored "-Wpedantic"
const struct audioDriver C6000_fmcopy_audio_driver = {
    .start = fmcopy_start,
    .data = fmcopy_data,
    .sync = fmcopy_sync,
    .stop = fmcopy_halt,
    .terminate = fmcopy_halt,
};
#pragma GCC diagnostic pop
