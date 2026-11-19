/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Receive baseband from the HR_C6000 FM receive copy: the chip demodulates the
 * 450 kHz IF itself and keeps the last 2 x 256 samples (8 kHz, 0-3.4 kHz,
 * big endian) in its data RAM at 0x030 (RadioFoundry modem-hrc6000.md 10.6).
 * A worker thread reads each half as the chip fills it, interpolates it to
 * 24 kHz and queues it; the stream takes its halves from that queue, so the
 * worker's 32 ms bursts never overwrite a half being read.
 *
 * VHF inverts the copy (the receive I/Q order follows the first LO's side):
 * the samples are negated there.
 */

#include <cmath>
#include <cstring>
#include <errno.h>
#include <pthread.h>
#include "interfaces/delays.h"
#include "drivers/baseband/HR_C6000.h"
#include "c6000_copy.h"

extern HR_C6000 C6000;
extern "C" bool radio_testRxVhf();

static constexpr uint16_t COPY_RAM   = 0x030;
static constexpr size_t   COPY_HALF  = 512;      // bytes, 256 samples
static constexpr size_t   PHASES     = 3;        // 8 kHz -> 24 kHz
static constexpr size_t   TAPS       = 12;       // per phase
static constexpr size_t   RING       = 4096;     // samples at 24 kHz
static constexpr int      SHIFT      = 4;        // copy +-16k -> +-1k

static const uint8_t saveRegs[] = { 0x41, 0x36, 0x60, 0x34, 0x10, 0x81 };
static uint8_t saved[sizeof(saveRegs)];

static float     taps[PHASES][TAPS];
static float     hist[TAPS];
static int16_t  *ring;
static volatile size_t rHead, rCount;
static struct streamCtx *volatile ctx;
static volatile bool running, stopReq;
static volatile uint8_t readyHalf, curHalf;
static pthread_t worker;
static pthread_mutex_t ringMtx = PTHREAD_MUTEX_INITIALIZER;
static uint32_t halves, fixes, drops;

/**
 * Windowed sinc low-pass at 3.6 kHz for the 24 kHz rate, gain 3.
 */
static void makeTaps()
{
    const size_t N = PHASES * TAPS;
    const float fc = 3600.0f / 24000.0f;
    for(size_t n = 0; n < N; n++)
    {
        float m = (float) n - (float) (N - 1) / 2.0f;
        float s = (m == 0.0f) ? 2.0f * fc
                              : sinf(2.0f * (float) M_PI * fc * m) / ((float) M_PI * m);
        float w = 0.54f - 0.46f * cosf(2.0f * (float) M_PI * (float) n / (float) (N - 1));
        taps[n % PHASES][n / PHASES] = 3.0f * s * w;
    }
}

static int16_t copyLast(const int half)
{
    uint8_t b[2];
    C6000.readRamExt(2, (uint16_t) (COPY_RAM + half * COPY_HALF + COPY_HALF - 2), b, 2);
    return (int16_t) ((b[0] << 8) | b[1]);
}

static void copyOn()
{
    for(size_t i = 0; i < sizeof(saveRegs); i++)
        saved[i] = C6000.readCfgRegister(saveRegs[i]);

    C6000.writeCfgRegister(0x41, 0x00);
    C6000.writeCfgRegister(0x36, 0x00);
    C6000.writeCfgRegister(0x60, 0x00);
    C6000.writeCfgRegister(0x34, 0x00);
    C6000.writeCfgRegister(0x10, 0x80);
    C6000.writeCfgRegister(0x81, (uint8_t) (saved[5] | 0x02));
    C6000.writeCfgRegister(0x83, 0xFF);
}

static void copyOff()
{
    for(size_t i = sizeof(saveRegs); i > 0; i--)
        C6000.writeCfgRegister(saveRegs[i - 1], saved[i - 1]);
    C6000.writeCfgRegister(0x83, 0xFF);
}

static void ringPush(const int16_t *s, const size_t n)
{
    pthread_mutex_lock(&ringMtx);
    for(size_t i = 0; i < n; i++)
    {
        if(rCount >= RING)
        {
            rHead = (rHead + 1) % RING;     // Drop the oldest
            rCount--;
            drops++;
        }
        ring[(rHead + rCount) % RING] = s[i];
        rCount++;
    }
    pthread_mutex_unlock(&ringMtx);
}

static void *workerFunc(void *arg)
{
    (void) arg;
    uint8_t raw[COPY_HALF];
    int16_t out[PHASES * COPY_HALF / 2];
    const bool invert = radio_testRxVhf();

    copyOn();
    int16_t last[2] = { copyLast(0), copyLast(1) };
    int lastHalf = 1;

    while(stopReq == false)
    {
        if((C6000.readCfgRegister(0x82) & 0x02) == 0)
        {
            sleepFor(0, 2);
            continue;
        }

        C6000.writeCfgRegister(0x83, 0x02);
        const int16_t now[2] = { copyLast(0), copyLast(1) };
        const bool ch0 = (now[0] != last[0]);
        const bool ch1 = (now[1] != last[1]);
        int h;
        if(ch0 && !ch1)      h = 0;
        else if(ch1 && !ch0) h = 1;
        else                 { h = lastHalf ^ 1; fixes++; }

        C6000.readRamExt(2, (uint16_t) (COPY_RAM + h * COPY_HALF), raw, COPY_HALF);
        last[0]  = now[0];
        last[1]  = now[1];
        lastHalf = h;
        halves++;

        size_t o = 0;
        for(size_t i = 0; i < COPY_HALF / 2; i++)
        {
            int16_t v = (int16_t) ((raw[2 * i] << 8) | raw[2 * i + 1]);
            float x = (float) (v >> SHIFT);
            if(invert)
                x = -x;

            memmove(&hist[1], &hist[0], (TAPS - 1) * sizeof(float));
            hist[0] = x;

            for(size_t p = 0; p < PHASES; p++)
            {
                float acc = 0.0f;
                for(size_t k = 0; k < TAPS; k++)
                    acc += taps[p][k] * hist[k];
                if(acc > 32767.0f)  acc = 32767.0f;
                if(acc < -32768.0f) acc = -32768.0f;
                out[o++] = (int16_t) acc;
            }
        }

        ringPush(out, o);
    }

    copyOff();
    return NULL;
}

static int copy_start(const uint8_t instance, const void *config,
                      struct streamCtx *c)
{
    (void) instance;
    (void) config;

    if((c == NULL) || (c->running != 0))
        return -EBUSY;

    if((c->sampleRate != 24000) || (c->bufMode != BUF_CIRC_DOUBLE)
       || ((c->bufSize % 2) != 0))
        return -EINVAL;

    if(ring == NULL)
    {
        ring = new int16_t[RING];
        makeTaps();
    }

    memset(hist, 0, sizeof(hist));
    rHead = rCount = 0;
    readyHalf = 0;
    curHalf = 0;
    stopReq = false;
    ctx = c;
    c->running = 1;
    running = true;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 6144);     // 2 kB of buffers in the worker
    if(pthread_create(&worker, &attr, workerFunc, NULL) != 0)
    {
        c->running = 0;
        running = false;
        return -ENOMEM;
    }

    return 0;
}

static int copy_data(struct streamCtx *c, stream_sample_t **buf)
{
    const size_t half = c->bufSize / 2;
    *buf = &c->buffer[readyHalf * half];
    return (int) half;
}

static int copy_sync(struct streamCtx *c, uint8_t dirty)
{
    (void) dirty;
    const size_t half = c->bufSize / 2;

    // Wait for a whole half in the queue (one copy half gives 768 samples)
    for(int i = 0; i < 200; i++)
    {
        if(running == false)
            return -1;

        pthread_mutex_lock(&ringMtx);
        bool ready = (rCount >= half);
        if(ready)
        {
            int16_t *dst = &c->buffer[curHalf * half];
            for(size_t k = 0; k < half; k++)
                dst[k] = ring[(rHead + k) % RING];
            rHead   = (rHead + half) % RING;
            rCount -= half;
        }
        pthread_mutex_unlock(&ringMtx);

        if(ready)
        {
            readyHalf = curHalf;
            curHalf  ^= 1;
            return 0;
        }

        sleepFor(0, 1);
    }

    return -1;
}

static void copy_halt(struct streamCtx *c)
{
    if(running == false)
        return;

    stopReq = true;
    pthread_join(worker, NULL);
    running = false;
    c->running = 0;
}

static void copy_stop(struct streamCtx *c)
{
    copy_halt(c);
}

void c6000copy_getStats(uint32_t *h, uint32_t *f, uint32_t *d)
{
    *h = halves;
    *f = fixes;
    *d = drops;
}

#pragma GCC diagnostic ignored "-Wpedantic"
const struct audioDriver c6000_copy_audio_driver =
{
    .start     = copy_start,
    .data      = copy_data,
    .sync      = copy_sync,
    .stop      = copy_stop,
    .terminate = copy_halt
};
#pragma GCC diagnostic pop
