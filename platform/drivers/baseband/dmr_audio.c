/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "hwconfig.h"
#include "dmr_audio.h"
#include "core/audio_stream.h"
#include "core/audio_path.h"
#include "core/threads.h"
#include <pthread.h>
#include <string.h>

#if defined(CONFIG_DMR)

#define FRAME_SAMPLES    160                    /* 20 ms at 8 kHz, one AMBE frame */
#define DMA_BUF_SAMPLES  (FRAME_SAMPLES * 2)    /* Double-buffered DMA            */
#define QUEUE_FRAMES     8                      /* Playback queue depth           */

static pathId  rxPath;
static bool    running  = false;
static bool    reqStop  = false;

/* Ring buffer of AMBE frames waiting to be decoded and played. */
static uint8_t queue[QUEUE_FRAMES][DMR_CODEC_FRAME_BYTES];
static uint8_t readPos;
static uint8_t writePos;
static uint8_t numFrames;

static pthread_t       thread;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * Playback thread: pull one AMBE frame per DMA slot, decode it into the idle
 * output buffer, and hand it to the stream. When the queue is empty the slot is
 * filled with silence so the DAC keeps running without underrunning.
 */
static void *playbackFunc(void *arg)
{
    pathId path = *((pathId *) arg);
    int16_t audioBuf[DMA_BUF_SAMPLES];

    /*
     * Bring the vocoder up here, on this thread's generous stack: the codec uses
     * far more of it than the rtx thread that starts playback has to spare.
     */
    if(dmr_codec_init() == false)
    {
        running = false;
        pthread_detach(pthread_self());
        return NULL;
    }

    memset(audioBuf, 0, sizeof(audioBuf));
    streamId os = audioStream_start(path, audioBuf, DMA_BUF_SAMPLES, 8000,
                                    STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    if(os < 0)
    {
        running = false;
        pthread_detach(pthread_self());
        return NULL;
    }

    outputStream_sync(os, false);

    while(reqStop == false)
    {
        if(audioPath_getStatus(path) != PATH_OPEN)
            break;

        uint8_t frame[DMR_CODEC_FRAME_BYTES];
        bool    haveFrame = false;

        pthread_mutex_lock(&mutex);
        if(numFrames > 0)
        {
            memcpy(frame, queue[readPos], DMR_CODEC_FRAME_BYTES);
            readPos = (readPos + 1) % QUEUE_FRAMES;
            numFrames -= 1;
            haveFrame = true;
        }
        pthread_mutex_unlock(&mutex);

        stream_sample_t *buf = outputStream_getIdleBuffer(os);
        if(buf == NULL)
            break;

        if(haveFrame)
            dmr_codec_decode(buf, frame);
        else
            memset(buf, 0, FRAME_SAMPLES * sizeof(stream_sample_t));

        outputStream_sync(os, true);
    }

    audioStream_stop(os);
    if(reqStop == false)
        pthread_detach(pthread_self());

    running = false;
    return NULL;
}

bool dmrAudio_startRx(void)
{
    if(running)
        return true;

    /* Cheap check here; the actual init runs on the playback thread's stack. */
    if(dmr_codec_available() == false)
        return false;

    rxPath = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_RX);
    if(audioPath_getStatus(rxPath) != PATH_OPEN)
    {
        audioPath_release(rxPath);
        return false;
    }

    readPos   = 0;
    writePos  = 0;
    numFrames = 0;
    reqStop   = false;
    running   = true;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
#if defined(_MIOSIX)
    pthread_attr_setstacksize(&attr, CODEC2_THREAD_STKSIZE);
    struct sched_param param;
    param.sched_priority = THREAD_PRIO_HIGH;
    pthread_attr_setschedparam(&attr, &param);
#endif

    if(pthread_create(&thread, &attr, playbackFunc, &rxPath) != 0)
    {
        running = false;
        audioPath_release(rxPath);
    }

    return running;
}

void dmrAudio_stopRx(void)
{
    if(running == false)
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
    if(running == false)
        return;

    pthread_mutex_lock(&mutex);
    if(numFrames < QUEUE_FRAMES)
    {
        memcpy(queue[writePos], frame, DMR_CODEC_FRAME_BYTES);
        writePos = (writePos + 1) % QUEUE_FRAMES;
        numFrames += 1;
    }
    pthread_mutex_unlock(&mutex);
}

#endif /* CONFIG_DMR */
