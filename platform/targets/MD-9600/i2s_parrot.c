/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "interfaces/delays.h"
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
        audioPath_release(path);
        return -2;
    }

    size_t n = 0;
    while(n < REC_SAMPLES)
    {
        dataBlock_t b = inputStream_getData(id);
        if((b.data == NULL) || (b.len == 0))
            break;

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
    pathId path = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_PROMPT);
    if(path < 0)
        return -3;

    // The HR_C6000 DAC takes whole 32-sample blocks
    streamId id = audioStream_start(path, rec, n & ~((size_t) 31), SAMPLE_RATE,
                                    STREAM_OUTPUT | BUF_LINEAR);
    if(id < 0)
    {
        audioPath_release(path);
        return -4;
    }

    audioStream_stop(id);
    audioPath_release(path);

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

void i2sParrot_readC6000(const uint8_t *regs, uint8_t *vals, size_t n);
void i2sParrot_writeC6000(const uint8_t reg, const uint8_t val);

#define NUM_EXP 4

/**
 * HR_C6000 set-up for a take, cycled one per take:
 * 0 as left by the radio, 1 vocoder encoder start (0x22 = 0x86), 2 and 3 the
 * same in DMR receive mode 0x10 = 0x4F (campaign) and 0x6E (RadioFoundry).
 *
 * @param exp: experiment number.
 * @param old10: the 0x10 value to restore, read before.
 */
static void expApply(const uint8_t exp)
{
    if(exp == 2) i2sParrot_writeC6000(0x10, 0x4F);
    if(exp == 3) i2sParrot_writeC6000(0x10, 0x6E);
    if(exp >= 1) i2sParrot_writeC6000(0x22, 0x86);
}

static void expRestore(const uint8_t exp, const uint8_t old10)
{
    if(exp >= 1) i2sParrot_writeC6000(0x22, 0x46);    // Encoder stop, I2S bits kept
    if(exp >= 2) i2sParrot_writeC6000(0x10, old10);
}

static void *parrotThread(void *arg)
{
    (void) arg;
    bool right = false;

    sleepFor(3, 0);

    while(1)
    {
        struct i2sStats st;

        static const uint8_t r10 = 0x10;
        uint8_t old10;
        const uint8_t exp = (uint8_t) (status.takes % NUM_EXP);

        i2sParrot_readC6000(&r10, &old10, 1);
        pthread_mutex_lock(&mutex);
        status.exp = exp;
        pthread_mutex_unlock(&mutex);

        stm32i2s_setChannel(right);
        expApply(exp);
        sleepFor(0, 50);
        stm32i2s_getStats(&st, true);
        setPhase(PARROT_REC, 0);

        int n = record();
        stm32i2s_getStats(&st, true);
        expRestore(exp, old10);

        pthread_mutex_lock(&mutex);
        status.stats = st;
        status.right = right;
        status.takes++;
        pthread_mutex_unlock(&mutex);

        if(n <= 0)
        {
            setPhase(PARROT_ERROR, (n < 0) ? n : -5);
            sleepFor(1, 0);
            continue;
        }

        const bool next = pickChannel(&st, right);
        right = next;

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

void i2sParrot_readC6000(const uint8_t *regs, uint8_t *vals, size_t n);

/**
 * Once a second: driver diagnostics to the display and the USB serial port.
 */
static void *diagThread(void *arg)
{
    (void) arg;
    static const uint8_t regs[] = { 0x06, 0x10, 0x22, 0x2F, 0x32, 0x33, 0x36,
                                    0xE0, 0xE2 };
    uint8_t vals[sizeof(regs)];
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

        printf("I2S t=%lu ph=%u takes=%lu irq=%lu ht=%lu tc=%lu te=%lu "
               "starts=%lu fails=%lu sr=%04lx cfg=%04lx dcr=%08lx ndtr=%lu "
               "lisr=%08lx ws=%lu ck=%lu sd=%lu/%lu frames=%lu "
               "L=%d/%u R=%d/%u noclk=%lu\r\n",
               (unsigned long) sec, phase, (unsigned long) takes,
               (unsigned long) d.irqs, (unsigned long) d.ht,
               (unsigned long) d.tc, (unsigned long) d.te,
               (unsigned long) d.starts, (unsigned long) d.startFails,
               (unsigned long) d.sr, (unsigned long) d.i2scfgr,
               (unsigned long) d.dmaCr, (unsigned long) d.ndtr,
               (unsigned long) d.lisr, (unsigned long) d.wsEdges,
               (unsigned long) d.ckEdges, (unsigned long) d.sdHigh,
               (unsigned long) d.polls, (unsigned long) st.frames,
               st.peak[0], st.rms[0], st.peak[1], st.rms[1],
               (unsigned long) st.noClock);

        if((sec % 2) == 0)
        {
            i2sParrot_readC6000(regs, vals, sizeof(regs));
            pthread_mutex_lock(&mutex);
            status.regs[0] = vals[1];   // 0x10
            status.regs[1] = vals[2];   // 0x22
            status.regs[2] = vals[7];   // 0xE0
            status.regs[3] = vals[6];   // 0x36
            status.regs[4] = vals[8];   // 0xE2
            status.regs[5] = vals[0];   // 0x06
            pthread_mutex_unlock(&mutex);
            printf("C6000");
            for(size_t i = 0; i < sizeof(regs); i++)
                printf(" %02x=%02x", regs[i], vals[i]);
            printf("\r\n");
        }

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
    pthread_attr_setstacksize(&attr, 2048);
    pthread_create(&thread, &attr, parrotThread, NULL);
    pthread_create(&thread, &attr, diagThread, NULL);
}

void i2sParrot_getStatus(struct parrotStatus *out)
{
    pthread_mutex_lock(&mutex);
    *out = status;
    pthread_mutex_unlock(&mutex);
}
