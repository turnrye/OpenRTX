/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "interfaces/delays.h"
#include "usb_vcom.h"
#include "i2s_parrot.h"

#define SAMPLE_RATE   8000
#define REC_SAMPLES   (3 * SAMPLE_RATE)
#define BLOCK_SAMPLES 160           // 20 ms per stream half
#define MAX_GAIN      16
#define PLAY_PEAK     16000         // Playback peak after the gain
#define SWITCH_RATIO  4             // Other channel this much louder: use it
#define SWITCH_MIN    30            // ... and above this RMS

static stream_sample_t rec[REC_SAMPLES];
static stream_sample_t block[2 * BLOCK_SAMPLES];
static struct parrotStatus status;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t serMutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * Write to the USB serial port, in pieces the CDC endpoint takes.
 * Caller holds serMutex.
 *
 * @return false if the host did not take the data (port not open).
 */
static bool serWrite(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *) buf;
    while(len > 0)
    {
        size_t n = (len > 512) ? 512 : len;
        if(vcom_writeBlock(p, n) != (ssize_t) n)
            return false;

        p   += n;
        len -= n;
    }

    return true;
}

/**
 * printf-style line to the USB serial port.
 */
static void serLog(const char *fmt, ...)
{
    char line[320];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);

    if(n < 0)
        return;
    if((size_t) n > sizeof(line) - 3)
        n = sizeof(line) - 3;

    line[n]     = '\r';
    line[n + 1] = '\n';

    pthread_mutex_lock(&serMutex);
    serWrite(line, n + 2);
    pthread_mutex_unlock(&serMutex);
}

/**
 * Dump a take as base64 between "PCM" and "END" lines, 48 bytes a line.
 * Stops at the first write the host does not take.
 */
static void serDump(const uint32_t take, const stream_sample_t *pcm, const size_t n)
{
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char hdr[48];
    char line[66];
    const uint8_t *src = (const uint8_t *) pcm;    // Little endian s16
    const size_t bytes = n * sizeof(stream_sample_t);

    int h = snprintf(hdr, sizeof(hdr), "PCM %lu %u 8000 s16le\r\n",
                     (unsigned long) take, (unsigned) n);

    pthread_mutex_lock(&serMutex);
    bool ok = serWrite(hdr, h);

    for(size_t off = 0; ok && (off < bytes); off += 48)
    {
        size_t chunk = ((bytes - off) > 48) ? 48 : (bytes - off);
        size_t o = 0;

        for(size_t i = 0; i < chunk; i += 3)
        {
            uint32_t v = (uint32_t) src[off + i] << 16;
            if((i + 1) < chunk) v |= (uint32_t) src[off + i + 1] << 8;
            if((i + 2) < chunk) v |= (uint32_t) src[off + i + 2];

            line[o++] = b64[(v >> 18) & 0x3F];
            line[o++] = b64[(v >> 12) & 0x3F];
            line[o++] = ((i + 1) < chunk) ? b64[(v >> 6) & 0x3F] : '=';
            line[o++] = ((i + 2) < chunk) ? b64[v & 0x3F] : '=';
        }

        line[o++] = '\r';
        line[o++] = '\n';
        ok = serWrite(line, o);
    }

    if(ok)
    {
        h = snprintf(hdr, sizeof(hdr), "END %lu\r\n", (unsigned long) take);
        serWrite(hdr, h);
    }

    pthread_mutex_unlock(&serMutex);
}

/**
 * Update the shared phase and error.
 */
static void setPhase(const uint8_t phase, const int16_t error)
{
    pthread_mutex_lock(&mutex);
    status.phase = phase;
    status.error = error;
    pthread_mutex_unlock(&mutex);
}

/**
 * Record up to REC_SAMPLES from the microphone.
 *
 * @return samples recorded, negative error code on failure.
 */
static int record()
{
    pathId path = audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX);
    if(path < 0)
        return -1;

    streamId id = audioStream_start(path, block, 2 * BLOCK_SAMPLES, SAMPLE_RATE,
                                    STREAM_INPUT | BUF_CIRC_DOUBLE);
    if(id < 0)
    {
        serLog("REC start failed %d", id);
        audioPath_release(path);
        return -2;
    }

    size_t n = 0;
    while(n < REC_SAMPLES)
    {
        dataBlock_t b = inputStream_getData(id);
        if((b.data == NULL) || (b.len == 0))
        {
            serLog("REC getData empty at %u", (unsigned) n);
            break;
        }

        size_t len = b.len;
        if(len > (REC_SAMPLES - n))
            len = REC_SAMPLES - n;

        memcpy(&rec[n], b.data, len * sizeof(stream_sample_t));
        n += len;
    }

    audioStream_terminate(id);
    audioPath_release(path);

    return (int) n;
}

/**
 * Remove the mean of the take and scale it up to PLAY_PEAK, at most MAX_GAIN.
 *
 * @param n: samples in the take.
 * @return gain applied.
 */
static uint8_t normalise(const size_t n)
{
    if(n == 0)
        return 1;

    int32_t sum = 0;
    for(size_t i = 0; i < n; i++)
        sum += rec[i];

    const int32_t mean = sum / (int32_t) n;
    int32_t peak = 0;
    for(size_t i = 0; i < n; i++)
    {
        int32_t a = rec[i] - mean;
        if(a < 0)
            a = -a;
        if(a > peak)
            peak = a;
    }

    int32_t gain = (peak > 0) ? (PLAY_PEAK / peak) : 1;
    if(gain < 1)
        gain = 1;
    if(gain > MAX_GAIN)
        gain = MAX_GAIN;

    for(size_t i = 0; i < n; i++)
    {
        int32_t s = (rec[i] - mean) * gain;
        if(s > INT16_MAX)
            s = INT16_MAX;
        if(s < INT16_MIN)
            s = INT16_MIN;
        rec[i] = (stream_sample_t) s;
    }

    return (uint8_t) gain;
}

/**
 * Play the take on the speaker, blocking until done.
 *
 * @param n: samples in the take.
 * @return 0 on success, negative error code on failure.
 */
static int play(const size_t n)
{
    long long t0 = getTick();

    pathId path = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_PROMPT);
    if(path < 0)
    {
        serLog("PLAY path request failed %ld", (long) path);
        return -3;
    }

    // Circular double buffer, as the Codec2 decoder does: audio_stream never
    // starts a linear output stream
    memset(block, 0, sizeof(block));
    streamId id = audioStream_start(path, block, 2 * BLOCK_SAMPLES, SAMPLE_RATE,
                                    STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    if(id < 0)
    {
        serLog("PLAY stream start failed %d (path %ld)", id, (long) path);
        audioPath_release(path);
        return -4;
    }

    long long t1 = getTick();
    outputStream_sync(id, false);

    size_t done = 0;
    uint32_t syncFails = 0;
    while(done < n)
    {
        stream_sample_t *buf = outputStream_getIdleBuffer(id);
        if(buf == NULL)
            break;

        size_t len = n - done;
        if(len > BLOCK_SAMPLES)
            len = BLOCK_SAMPLES;

        memcpy(buf, &rec[done], len * sizeof(stream_sample_t));
        if(len < BLOCK_SAMPLES)
            memset(&buf[len], 0, (BLOCK_SAMPLES - len) * sizeof(stream_sample_t));

        done += len;
        if(outputStream_sync(id, true) == false)
            syncFails++;
    }

    audioStream_stop(id);
    long long t2 = getTick();
    audioPath_release(path);

    serLog("PLAY path %ld stream %d: %u of %u samples, %ld ms, %lu sync fails",
           (long) path, id, (unsigned) done, (unsigned) n, (long) (t2 - t1),
           (unsigned long) syncFails);
    (void) t0;

    return 0;
}

/**
 * Pick the channel for the next take: switch when the other one is clearly
 * louder.
 */
static bool pickChannel(const struct i2sStats *st, const bool right)
{
    const uint8_t cur   = right ? 1 : 0;
    const uint8_t other = right ? 0 : 1;

    if((st->rms[other] > SWITCH_MIN) &&
       (st->rms[other] > (SWITCH_RATIO * (uint32_t) st->rms[cur])))
        return !right;

    return right;
}

void i2sParrot_writeC6000(const uint8_t reg, const uint8_t val);

#define NUM_VARIANTS 4

/**
 * HR_C6000 set-up before a take, one per take: 0 nothing, 1 and 2 vocoder
 * encoder start (0x22 = 0x86), 3 register 0x06 back to its init value 0x21.
 */
static void variantBefore(const uint8_t v)
{
    if((v == 1) || (v == 2)) i2sParrot_writeC6000(0x22, 0x86);
    if(v == 3)               i2sParrot_writeC6000(0x06, 0x21);
}

/**
 * Undo after a take: 2 stops the encoder (0x22 = 0x46).
 */
static void variantAfter(const uint8_t v)
{
    if(v == 2) i2sParrot_writeC6000(0x22, 0x46);
}

static void *parrotThread(void *arg)
{
    (void) arg;
    bool right = false;

    sleepFor(3, 0);
    serLog("PARROT start");

    while(1)
    {
        struct i2sStats st;
        uint32_t take;

        pthread_mutex_lock(&mutex);
        take = status.takes;
        pthread_mutex_unlock(&mutex);

        const uint8_t v = (uint8_t) (take % NUM_VARIANTS);
        pthread_mutex_lock(&mutex);
        status.exp = v;
        pthread_mutex_unlock(&mutex);

        stm32i2s_setChannel(right);
        variantBefore(v);
        sleepFor(0, 50);
        stm32i2s_getStats(&st, true);
        setPhase(PARROT_REC, 0);

        long long t0 = getTick();
        int n = record();
        long long t1 = getTick();
        stm32i2s_getStats(&st, true);
        variantAfter(v);

        pthread_mutex_lock(&mutex);
        status.stats = st;
        status.right = right;
        status.takes++;
        pthread_mutex_unlock(&mutex);

        serLog("TAKE %lu V%u n=%d %ld ms ch=%c frames=%lu L=%d/%u mean %d "
               "R=%d/%u mean %d", (unsigned long) take, v, n, (long) (t1 - t0),
               right ? 'R' : 'L', (unsigned long) st.frames, st.peak[0],
               st.rms[0], st.mean[0], st.peak[1], st.rms[1], st.mean[1]);

        if(n <= 0)
        {
            setPhase(PARROT_ERROR, (n < 0) ? n : -5);
            sleepFor(1, 0);
            continue;
        }

        serDump(take, rec, (size_t) n);
        right = pickChannel(&st, right);

        uint8_t gain = normalise((size_t) n);
        pthread_mutex_lock(&mutex);
        status.gain = gain;
        pthread_mutex_unlock(&mutex);

        setPhase(PARROT_PLAY, 0);
        int ret = play((size_t) n);
        setPhase((ret < 0) ? PARROT_ERROR : PARROT_IDLE, (int16_t) ret);

        sleepFor(0, 500);
    }

    return NULL;
}

/**
 * Once a second: driver diagnostics to the display and the USB serial port.
 */
static void *diagThread(void *arg)
{
    (void) arg;
    uint32_t sec = 0;

    sleepFor(2, 0);

    while(1)
    {
        struct i2sDiag d;
        struct i2sStats st;
        stm32i2s_getDiag(&d);
        stm32i2s_getStats(&st, false);

        pthread_mutex_lock(&mutex);
        status.diag = d;
        status.live = st;
        const uint8_t phase = status.phase;
        const uint32_t takes = status.takes;
        pthread_mutex_unlock(&mutex);

        serLog("I2S t=%lu ph=%u takes=%lu irq=%lu to=%lu pos=%lu run=%lu "
               "sr=%04lx ndtr=%lu ws=%lu ck=%lu sd=%lu/%lu frames=%lu "
               "L=%d/%u R=%d/%u noclk=%lu",
               (unsigned long) sec, phase, (unsigned long) takes,
               (unsigned long) d.irqs, (unsigned long) d.timeouts,
               (unsigned long) d.pos, (unsigned long) d.running,
               (unsigned long) d.sr, (unsigned long) d.ndtr,
               (unsigned long) d.wsEdges, (unsigned long) d.ckEdges,
               (unsigned long) d.sdHigh, (unsigned long) d.polls,
               (unsigned long) st.frames, st.peak[0], st.rms[0], st.peak[1],
               st.rms[1], (unsigned long) st.noClock);

        sec++;
        sleepFor(1, 0);
    }

    return NULL;
}

void i2sParrot_start()
{
    pthread_attr_t attr;
    pthread_t thread;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4096);
    pthread_create(&thread, &attr, parrotThread, NULL);
    pthread_create(&thread, &attr, diagThread, NULL);
}

void i2sParrot_getStatus(struct parrotStatus *out)
{
    pthread_mutex_lock(&mutex);
    *out = status;
    pthread_mutex_unlock(&mutex);
}
