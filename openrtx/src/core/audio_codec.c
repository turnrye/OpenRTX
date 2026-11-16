/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/audio_stream.h"
#include "core/audio_codec.h"
#include <pthread.h>
#include "core/threads.h"
// codec2 system library has a weird include prefix
#if defined(PLATFORM_LINUX)
#include <codec2/codec2.h>
#else
#include "codec2.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "core/dsp.h"
#include "core/utils.h"
#include "hwconfig.h"

#define BUF_SIZE 4
#define CODEC2_FRAME_SAMPLES 160 /* Samples per codec2 3200-mode frame */
#define DMA_BUF_SAMPLES (CODEC2_FRAME_SAMPLES * 2) /* Double-buffered DMA */

#ifndef CONFIG_MIC_GAIN
#define CONFIG_MIC_GAIN 1
#endif

#ifndef CONFIG_MIC_OVERSAMPLE
#define CONFIG_MIC_OVERSAMPLE 1
#endif

#ifdef MIC_EXP_H7_OVS
/*
 * EXPERIMENT (private): with 16 bit x16 hardware oversampling the mic samples
 * are 15 bit, eight times the 12 bit scale: divide the gain by eight so the
 * overall gain is unchanged. Not combined with MIC_EXP_KEEP_SUM, whose sum of
 * 15 bit samples would overflow int16.
 */
#if defined(MIC_EXP_KEEP_SUM)
#error "MIC_EXP_H7_OVS and MIC_EXP_KEEP_SUM cannot be combined"
#endif
#if (CONFIG_MIC_GAIN % 8) != 0
#error "MIC_EXP_H7_OVS needs CONFIG_MIC_GAIN divisible by 8"
#endif
#define MIC_GAIN (CONFIG_MIC_GAIN / 8)
#else
#define MIC_GAIN CONFIG_MIC_GAIN
#endif

/*
 * EXPERIMENT (private/mic-exp): the high-pass and the AGC run on a float copy
 * of the block, with the base gain applied without saturation.
 */
#if defined(MIC_EXP_HPF) || defined(MIC_EXP_AGC)
#define MIC_EXP_FLOAT_CHAIN
#include <math.h>
#if defined(MIC_EXP_KEEP_SUM) && (CONFIG_MIC_OVERSAMPLE > 1)
#define MIC_EXP_SCALE \
    ((float)MIC_GAIN / ((float)CONFIG_MIC_OVERSAMPLE * 32768.0f))
#else
#define MIC_EXP_SCALE ((float)MIC_GAIN / 32768.0f)
#endif
#endif

#ifndef MIC_EXP_HPF_HZ
#define MIC_EXP_HPF_HZ 110.0f
#endif

#ifdef MIC_EXP_AGC
// Kept across transmissions, so each over starts at the gain the last ended on
static struct agcState micAgc;
static bool micAgcReady = false;
#endif

static pathId audioPath;

static uint8_t initCnt = 0;
static bool running;

static bool reqStop;
static pthread_t codecThread;
static pthread_attr_t codecAttr;
static pthread_mutex_t data_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wakeup_cond = PTHREAD_COND_INITIALIZER;

static uint8_t readPos;
static uint8_t writePos;
static uint8_t numElements;
static uint64_t dataBuffer[BUF_SIZE];

static void *encodeFunc(void *arg);
static void *decodeFunc(void *arg);
static bool startThread(const pathId path, void *(*func)(void *));
static void stopThread();

void codec_init()
{
    pthread_mutex_lock(&init_mutex);
    initCnt += 1;
    pthread_mutex_unlock(&init_mutex);

    if (initCnt > 0)
        return;

    running = false;
    readPos = 0;
    writePos = 0;
    numElements = 0;
}

void codec_terminate()
{
    pthread_mutex_lock(&init_mutex);
    initCnt -= 1;
    pthread_mutex_unlock(&init_mutex);

    if (initCnt > 0)
        return;

    if (running)
        stopThread();
}

bool codec_startEncode(const pathId path)
{
    return startThread(path, encodeFunc);
}

bool codec_startDecode(const pathId path)
{
    return startThread(path, decodeFunc);
}

void codec_stop(const pathId path)
{
    if (running == false)
        return;

    if (audioPath != path)
        return;

    stopThread();
}

bool codec_running()
{
    return running;
}

bool codec_queueEmpty(const pathId path)
{
    if ((running == false) || (audioPath != path))
        return true;

    return numElements == 0;
}

int codec_popFrame(uint8_t *frame, const bool blocking)
{
    if (running == false)
        return -EPERM;

    uint64_t element;

    // No data available and non-blocking call: just return false.
    if ((numElements == 0) && (blocking == false))
        return -EAGAIN;

    // Blocking call: wait until some data is pushed
    pthread_mutex_lock(&data_mutex);
    while (numElements == 0) {
        pthread_cond_wait(&wakeup_cond, &data_mutex);
    }

    element = dataBuffer[readPos];
    readPos = (readPos + 1) % BUF_SIZE;
    numElements -= 1;
    pthread_mutex_unlock(&data_mutex);

    // Do memcpy after mutex unlock to reduce execution time spent inside the
    // critical section
    memcpy(frame, &element, 8);

    return 0;
}

int codec_pushFrame(const uint8_t *frame, const bool blocking)
{
    if (running == false)
        return -EPERM;

    // Copy data to a temporary variable before mutex lock to reduce execution
    // time spent inside the critical section
    uint64_t element;
    memcpy(&element, frame, 8);

    // No space available and non-blocking call: return
    if ((numElements >= BUF_SIZE) && (blocking == false))
        return -EAGAIN;

    // Blocking call: wait until there is some free space
    pthread_mutex_lock(&data_mutex);
    while (numElements >= BUF_SIZE) {
        pthread_cond_wait(&wakeup_cond, &data_mutex);
    }

    // There is free space, push data into the queue
    dataBuffer[writePos] = element;
    writePos = (writePos + 1) % BUF_SIZE;
    numElements += 1;

    pthread_mutex_unlock(&data_mutex);
    return 0;
}

static void *encodeFunc(void *arg)
{
    streamId iStream;
    pathId iPath = *((pathId *)arg);
    struct CODEC2 *codec2;
    struct dcBlock dcBlock;
    struct decimatorState decimator;

    // Allocate on the heap, as the stack isn't big enough for oversampling
    // above 2 or so.
    stream_sample_t *adcBuf = malloc(DMA_BUF_SAMPLES * CONFIG_MIC_OVERSAMPLE
                                     * sizeof(stream_sample_t));
    if (adcBuf == NULL)
        goto exit;

    iStream = audioStream_start(iPath, adcBuf,
                                DMA_BUF_SAMPLES * CONFIG_MIC_OVERSAMPLE,
                                8000 * CONFIG_MIC_OVERSAMPLE,
                                STREAM_INPUT | BUF_CIRC_DOUBLE);
    if (iStream < 0)
        goto exit;

    dsp_resetState(dcBlock);
    dsp_resetState(decimator);
    codec2 = codec2_create(CODEC2_MODE_3200);
#ifdef MIC_EXP_PRIME_DC
    bool primeDc = true;
#endif
#ifdef MIC_EXP_FLOAT_CHAIN
    float fbuf[CODEC2_FRAME_SAMPLES];
#endif
#ifdef MIC_EXP_HPF
    struct biquad hpf;
    dsp_highPassInit(&hpf, MIC_EXP_HPF_HZ, 8000.0f);
#endif
#ifdef MIC_EXP_AGC
    if (micAgcReady == false) {
        struct agcParams params;
        dsp_agcDefaults(&params);
#ifdef MIC_EXP_AGC_KNEE
        params.kneeDb = MIC_EXP_AGC_KNEE;
#endif
#ifdef MIC_EXP_AGC_ATTACK
        params.attackDbPerBlk = MIC_EXP_AGC_ATTACK;
#endif
#ifdef MIC_EXP_AGC_FASTDOWN
        params.fastDownDb = MIC_EXP_AGC_FASTDOWN;
#endif
#ifdef MIC_EXP_AGC_TARGET
        params.targetDb = MIC_EXP_AGC_TARGET;
#endif
        dsp_agcInit(&micAgc, &params);
        micAgcReady = true;
    }
#endif

    while (reqStop == false) {
        // Invalid audio path, quit
        if (audioPath_getStatus(iPath) != PATH_OPEN)
            break;

        dataBlock_t audio = inputStream_getData(iStream);
        if (audio.data == NULL || audio.len == 0)
            break;

        // Each inputStream_getData call returns exactly
        // CODEC2_FRAME_SAMPLES * CONFIG_MIC_OVERSAMPLE raw samples
        // (one half of the double-buffered DMA transfer).  Decimate
        // by averaging each group of CONFIG_MIC_OVERSAMPLE samples.
        size_t outIdx = 0;
        for (size_t i = 0; i < audio.len; i++) {
            int16_t sample = audio.data[i];

#if CONFIG_MIC_OVERSAMPLE > 1
#ifdef MIC_EXP_KEEP_SUM
            // EXPERIMENT: emit the sum of the oversampled block, keeping the
            // resolution the averaging gains; the gain is divided by N below.
            decimator.accumulator += sample;
            decimator.count++;
            if (decimator.count < CONFIG_MIC_OVERSAMPLE)
                continue;
            sample = (int16_t)decimator.accumulator;
            decimator.accumulator = 0;
            decimator.count = 0;
#else
            if (!dsp_decimator(&decimator, &sample, CONFIG_MIC_OVERSAMPLE))
                continue;
#endif
#endif

#ifdef MIC_EXP_PRIME_DC
            // EXPERIMENT: start the DC block settled on the first sample, so
            // the ADC bias does not pass through as a key-up click.
            if (primeDc) {
                dcBlock.prevIn = (int32_t)sample * 32768;
                primeDc = false;
            }
#endif

            sample = dsp_dcBlockFilter(&dcBlock, sample);
#ifdef MIC_EXP_FLOAT_CHAIN
            // EXPERIMENT: base gain in float, without saturation, so the
            // HPF/AGC see loud input unclipped.
            fbuf[outIdx++] = (float)sample * MIC_EXP_SCALE;
#elif defined(MIC_EXP_KEEP_SUM) && (CONFIG_MIC_OVERSAMPLE > 1)
            int32_t scaled = ((int32_t)sample * MIC_GAIN)
                           / CONFIG_MIC_OVERSAMPLE;
            if (scaled > INT16_MAX)
                scaled = INT16_MAX;
            if (scaled < INT16_MIN)
                scaled = INT16_MIN;
            audio.data[outIdx++] = (int16_t)scaled;
#else
            audio.data[outIdx++] = s16_mulSat(sample, MIC_GAIN);
#endif
        }

#ifdef MIC_EXP_FLOAT_CHAIN
#ifdef MIC_EXP_HPF
        for (size_t i = 0; i < outIdx; i++)
            fbuf[i] = dsp_biquad(&hpf, fbuf[i]);
#endif
#ifdef MIC_EXP_AGC
        dsp_agcProcess(&micAgc, fbuf, outIdx);
#endif
        for (size_t i = 0; i < outIdx; i++) {
            float v = fbuf[i] * 32767.0f;
            if (v > 32767.0f)
                v = 32767.0f;
            if (v < -32768.0f)
                v = -32768.0f;
            audio.data[i] = (int16_t)lrintf(v);
        }
#endif

        // CODEC2 encodes 160 speech samples (20 ms), into 8 bytes: here we
        // write the new encoded data into a buffer of 16 bytes, writing the
        // first half and then the second one, sequentially.
        // Data ready flag is rised once all the 16 bytes contain new data.
        uint64_t frame = 0;
#if defined(PLATFORM_LINUX)
        // EXPERIMENT (private): emulator tap of the exact samples codec2
        // encodes, appended as raw s16le, when OPENRTX_MIC_PRECODEC_FILE is set
        {
            const char *tap = getenv("OPENRTX_MIC_PRECODEC_FILE");
            if ((tap != NULL) && (tap[0] != '\0')) {
                FILE *fp = fopen(tap, "ab");
                if (fp != NULL) {
                    fwrite(audio.data, sizeof(int16_t), CODEC2_FRAME_SAMPLES,
                           fp);
                    fclose(fp);
                }
            }
        }
#endif
        codec2_encode(codec2, ((uint8_t *)&frame), audio.data);

        pthread_mutex_lock(&data_mutex);

        // If buffer is full erase the oldest frame
        if (numElements >= BUF_SIZE) {
            readPos = (readPos + 1) % BUF_SIZE;
        }

        dataBuffer[writePos] = frame;
        writePos = (writePos + 1) % BUF_SIZE;

        if (numElements == 0)
            pthread_cond_signal(&wakeup_cond);

        if (numElements < BUF_SIZE)
            numElements += 1;

        pthread_mutex_unlock(&data_mutex);
    }

    audioStream_terminate(iStream);
    codec2_destroy(codec2);

exit:
    free(adcBuf);

    // In case thread terminates due to invalid path or stream error, detach it
    // to ensure that its memory gets freed by the OS.
    if (reqStop == false)
        pthread_detach(pthread_self());

    running = false;
    return NULL;
}

static void *decodeFunc(void *arg)
{
    streamId oStream;
    pathId oPath = *((pathId *)arg);
    int16_t audioBuf[DMA_BUF_SAMPLES];
    struct CODEC2 *codec2;

    // Open output stream
    memset(audioBuf, 0x00, DMA_BUF_SAMPLES * sizeof(stream_sample_t));
    oStream = audioStream_start(oPath, audioBuf, DMA_BUF_SAMPLES, 8000,
                                STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    if (oStream < 0) {
        pthread_detach(pthread_self());
        running = false;
        return NULL;
    }

    codec2 = codec2_create(CODEC2_MODE_3200);

    // Ensure that thread start is correctly synchronized with the output
    // stream to avoid having the decode function writing in a memory area
    // being read at the same time by the output stream system causing cracking
    // noises at speaker output. Behaviour observed on both Module17 and MD-UV380
    outputStream_sync(oStream, false);

    while (reqStop == false) {
        // Invalid path, quit
        if (audioPath_getStatus(oPath) != PATH_OPEN)
            break;

        // Try popping data from the queue
        uint64_t frame = 0;
        bool newData = false;

        pthread_mutex_lock(&data_mutex);

        if (numElements != 0) {
            frame = dataBuffer[readPos];
            readPos = (readPos + 1) % BUF_SIZE;
            if (numElements >= BUF_SIZE)
                pthread_cond_signal(&wakeup_cond);

            numElements -= 1;
            newData = true;
        }

        pthread_mutex_unlock(&data_mutex);

        stream_sample_t *audioBuf = outputStream_getIdleBuffer(oStream);
        if (audioBuf == NULL)
            break;

        if (newData) {
            codec2_decode(codec2, audioBuf, ((uint8_t *)&frame));

#ifdef PLATFORM_MD3x0
            // Bump up volume a little bit, as on MD3x0 is quite low
            for (size_t i = 0; i < CODEC2_FRAME_SAMPLES; i++)
                audioBuf[i] *= 2;
#endif

        } else {
            memset(audioBuf, 0x00, CODEC2_FRAME_SAMPLES * sizeof(int16_t));
        }

        outputStream_sync(oStream, true);
    }

    // Stop stream and wait until its effective termination
    audioStream_stop(oStream);
    codec2_destroy(codec2);

    // In case thread terminates due to invalid path or stream error, detach it
    // to ensure that its memory gets freed by the OS.
    if (reqStop == false)
        pthread_detach(pthread_self());

    running = false;
    return NULL;
}

static bool startThread(const pathId path, void *(*func)(void *))
{
    // Bad incoming path
    if (audioPath_getStatus(path) != PATH_OPEN)
        return false;

    // Handle access contention when starting the codec thread to ensure that
    // only one call at a time can effectively start the thread.
    pthread_mutex_lock(&init_mutex);
    if (running) {
        // Same path as before, path open, codec already running: all good.
        if (path == audioPath) {
            pthread_mutex_unlock(&init_mutex);
            return true;
        }

        // New path takes over the current one only if it has an higher priority
        // or the current one is closed/suspended.
        pathInfo_t newPath = audioPath_getInfo(path);
        pathInfo_t curPath = audioPath_getInfo(audioPath);
        if ((curPath.status == PATH_OPEN) && (curPath.prio >= newPath.prio)) {
            pthread_mutex_unlock(&init_mutex);
            return false;
        } else {
            stopThread();
        }
    }

    running = true;
    audioPath = path;
    pthread_mutex_unlock(&init_mutex);

    readPos = 0;
    writePos = 0;
    numElements = 0;
    reqStop = false;

    pthread_attr_init(&codecAttr);

#if defined(_MIOSIX)
    // Set stack size of CODEC2 thread to 16kB.
    pthread_attr_setstacksize(&codecAttr, CODEC2_THREAD_STKSIZE);

    // Set priority of CODEC2 thread to the maximum one, the same of RTX thread.
    struct sched_param param;
    param.sched_priority = THREAD_PRIO_HIGH;
    pthread_attr_setschedparam(&codecAttr, &param);
#elif defined(__ZEPHYR__)
    // Allocate and set the stack for CODEC2 thread
    void *codec_thread_stack = malloc(CODEC2_THREAD_STKSIZE * sizeof(uint8_t));
    pthread_attr_setstack(&codecAttr, codec_thread_stack,
                          CODEC2_THREAD_STKSIZE);
#endif

    // Start thread
    int ret = pthread_create(&codecThread, &codecAttr, func, &audioPath);
    if (ret < 0)
        running = false;

    return running;
}

static void stopThread()
{
    reqStop = true;
    pthread_join(codecThread, NULL);
    running = false;

#ifdef __ZEPHYR__
    void *addr;
    size_t size;

    pthread_attr_getstack(&codecAttr, &addr, &size);
    free(addr);
#endif
}
