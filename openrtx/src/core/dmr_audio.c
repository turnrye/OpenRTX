/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "hwconfig.h"
#include "core/dmr_audio.h"
#include "core/mic_frame.h"
#include "protocols/DMR/VoiceFrame.h"
#include "core/audio_stream.h"
#include "core/audio_path.h"
#include "core/threads.h"
#include "core/dsp.h"
#include "core/dmr_debug.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#if defined(CONFIG_DMR)

#define FRAME_SAMPLES 160                   /* 20 ms at 8 kHz, one AMBE frame */
#define DMA_BUF_SAMPLES (FRAME_SAMPLES * 2) /* Double-buffered DMA            */
#define QUEUE_FRAMES 8                      /* Playback queue depth           */

#ifndef CONFIG_MIC_OVERSAMPLE
#define CONFIG_MIC_OVERSAMPLE 1
#endif

static pathId rxPath;
static bool running = false;
static bool reqStop = false;

/* Ring buffer of AMBE frames waiting to be decoded and played. */
static uint8_t queue[QUEUE_FRAMES][DMR_CODEC_FRAME_BYTES];
static uint8_t readPos;
static uint8_t writePos;
static uint8_t numFrames;

static pthread_t thread;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * Playback thread: pull one AMBE frame per DMA slot, decode it into the idle
 * output buffer, and hand it to the stream. When the queue is empty the slot is
 * filled with silence so the DAC keeps running without underrunning.
 */
static void *playbackFunc(void *arg)
{
    pathId path = *((pathId *)arg);
    int16_t audioBuf[DMA_BUF_SAMPLES];

    /*
     * Bring the vocoder up here, on this thread's generous stack: the codec uses
     * far more of it than the rtx thread that starts playback has to spare.
     */
    if (dmr_codec_init() == false) {
        dmrDebug_log(DMR_DBG_INFO, "audio: playback not started, no vocoder");
        running = false;
        pthread_detach(pthread_self());
        return NULL;
    }

    memset(audioBuf, 0, sizeof(audioBuf));
    streamId os = audioStream_start(path, audioBuf, DMA_BUF_SAMPLES, 8000,
                                    STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    if (os < 0) {
        dmrDebug_log(DMR_DBG_INFO,
                     "audio: playback not started, output "
                     "stream error %d",
                     (int)os);
        running = false;
        pthread_detach(pthread_self());
        return NULL;
    }

    outputStream_sync(os, false);

    while (reqStop == false) {
        if (audioPath_getStatus(path) != PATH_OPEN) {
            dmrDebug_log(DMR_DBG_INFO, "audio: playback stopped, speaker path "
                                       "taken over");
            break;
        }

        uint8_t frame[DMR_CODEC_FRAME_BYTES];
        bool haveFrame = false;

        pthread_mutex_lock(&mutex);
        if (numFrames > 0) {
            memcpy(frame, queue[readPos], DMR_CODEC_FRAME_BYTES);
            readPos = (readPos + 1) % QUEUE_FRAMES;
            numFrames -= 1;
            haveFrame = true;
        }
        pthread_mutex_unlock(&mutex);

        stream_sample_t *buf = outputStream_getIdleBuffer(os);
        if (buf == NULL) {
            dmrDebug_log(DMR_DBG_INFO, "audio: playback stopped, output "
                                       "stream lost");
            break;
        }

        if (haveFrame)
            dmr_codec_decode(buf, frame);
        else
            memset(buf, 0, FRAME_SAMPLES * sizeof(stream_sample_t));

        outputStream_sync(os, true);
    }

    audioStream_stop(os);
    if (reqStop == false)
        pthread_detach(pthread_self());

    running = false;
    return NULL;
}

bool dmrAudio_startRx(void)
{
    if (running)
        return true;

    /* Cheap check here; the actual init runs on the playback thread's stack. */
    if (dmr_codec_available() == false)
        return false;

    rxPath = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_RX);
    if (audioPath_getStatus(rxPath) != PATH_OPEN) {
        audioPath_release(rxPath);
        return false;
    }

    readPos = 0;
    writePos = 0;
    numFrames = 0;
    reqStop = false;
    running = true;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
#if defined(_MIOSIX)
    pthread_attr_setstacksize(&attr, CODEC2_THREAD_STKSIZE);
    struct sched_param param;
    param.sched_priority = THREAD_PRIO_HIGH;
    pthread_attr_setschedparam(&attr, &param);
#endif

    if (pthread_create(&thread, &attr, playbackFunc, &rxPath) != 0) {
        running = false;
        audioPath_release(rxPath);
    }

    return running;
}

void dmrAudio_stopRx(void)
{
    if (running == false)
        return;

    reqStop = true;
    pthread_join(thread, NULL);
    audioPath_release(rxPath);
    running = false;
}

bool dmrAudio_running(void)
{
    return running;
}

void dmrAudio_pushFrame(const uint8_t frame[DMR_CODEC_FRAME_BYTES])
{
    if (running == false)
        return;

    pthread_mutex_lock(&mutex);
    if (numFrames < QUEUE_FRAMES) {
        memcpy(queue[writePos], frame, DMR_CODEC_FRAME_BYTES);
        writePos = (writePos + 1) % QUEUE_FRAMES;
        numFrames += 1;
    }
    pthread_mutex_unlock(&mutex);
}

/*
 * Transmit side: capture the microphone, encode it and hand the AMBE frames to
 * the baseband, which takes them on its own thread as each voice burst goes
 * out. The queue is a single-producer/single-consumer ring with no lock, so
 * that taking a frame never holds the baseband up: head is written only by the
 * encode thread, tail only by the baseband; both are word-aligned reads and
 * writes. The baseband takes frames only while capture runs, so starting and
 * stopping it can reset the queue.
 */
#define TX_QUEUE_FRAMES 8

static uint8_t txQueue[TX_QUEUE_FRAMES][DMR_CODEC_FRAME_BYTES];
static volatile uint32_t txHead;
static volatile uint32_t txTail;
static pathId txPath;
static bool txRunning = false;
static bool txReqStop = false;
static pthread_t txThread;

/**
 * Capture/encode thread: pull one 20 ms chunk of microphone audio at a time,
 * decimate it to 8 kHz mono, encode it to a 9-byte AMBE frame and queue it.
 */
static void *captureFunc(void *arg)
{
    pathId path = *((pathId *)arg);
    struct micFrameState mic;

    /* Oversampled ADC buffer is too big for the stack. */
    stream_sample_t *adcBuf = malloc(DMA_BUF_SAMPLES * CONFIG_MIC_OVERSAMPLE
                                     * sizeof(stream_sample_t));
    if (adcBuf == NULL)
        goto detach;

    if (dmr_codec_init() == false)
        goto freebuf;

    streamId is = audioStream_start(path, adcBuf,
                                    DMA_BUF_SAMPLES * CONFIG_MIC_OVERSAMPLE,
                                    8000 * CONFIG_MIC_OVERSAMPLE,
                                    STREAM_INPUT | BUF_CIRC_DOUBLE);
    if (is < 0)
        goto freebuf;

    micFrame_init(&mic);

    while (txReqStop == false) {
        if (audioPath_getStatus(path) != PATH_OPEN)
            break;

        dataBlock_t audio = inputStream_getData(is);
        if ((audio.data == NULL) || (audio.len == 0))
            break;

        /* One buffer is FRAME_SAMPLES*oversample raw samples; decimate to 160. */
        size_t n = micFrame_condition(&mic, audio.data, audio.len);

        /* Without voice the silence frame goes out, as other radios send. */
        uint8_t frame[DMR_CODEC_FRAME_BYTES];
        if (micFrame_hasVoice(&mic, audio.data, n))
            dmr_codec_encode(frame, audio.data);
        else
            memcpy(frame, DMR_VOICE_SILENCE_FRAME, DMR_CODEC_FRAME_BYTES);

        if ((txHead - txTail) < TX_QUEUE_FRAMES) {
            memcpy(txQueue[txHead % TX_QUEUE_FRAMES], frame,
                   DMR_CODEC_FRAME_BYTES);
            __sync_synchronize();
            txHead += 1;
        }
    }

    audioStream_terminate(is);

freebuf:
    free(adcBuf);
detach:
    if (txReqStop == false)
        pthread_detach(pthread_self());
    txRunning = false;
    return NULL;
}

bool dmrAudio_startTx(void)
{
    if (txRunning)
        return true;

    if (dmr_codec_available() == false)
        return false;

    txPath = audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX);
    if (audioPath_getStatus(txPath) != PATH_OPEN) {
        audioPath_release(txPath);
        return false;
    }

    txHead = 0;
    txTail = 0;
    txReqStop = false;
    txRunning = true;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
#if defined(_MIOSIX)
    pthread_attr_setstacksize(&attr, CODEC2_THREAD_STKSIZE);
    struct sched_param param;
    param.sched_priority = THREAD_PRIO_HIGH;
    pthread_attr_setschedparam(&attr, &param);
#endif

    if (pthread_create(&txThread, &attr, captureFunc, &txPath) != 0) {
        txRunning = false;
        audioPath_release(txPath);
    }

    return txRunning;
}

void dmrAudio_stopTx(void)
{
    if (txRunning == false)
        return;

    txReqStop = true;
    pthread_join(txThread, NULL);
    audioPath_release(txPath);

    /* Frames encoded for a transmission that is over are not handed out. */
    txTail = txHead;
    txRunning = false;
}

bool dmrAudio_txRunning(void)
{
    return txRunning;
}

bool dmrAudio_popFrame(uint8_t frame[DMR_CODEC_FRAME_BYTES])
{
    if (txHead == txTail)
        return false;

    memcpy(frame, txQueue[txTail % TX_QUEUE_FRAMES], DMR_CODEC_FRAME_BYTES);
    __sync_synchronize();
    txTail += 1;
    return true;
}

#endif /* CONFIG_DMR */
