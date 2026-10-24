/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/tones.h"
#include "core/audio_path.h"
#include "core/threads.h"
#include "core/voicePrompts.h"
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include <pthread.h>
#include <string.h>

#define TONES_THREAD_STKSIZE 1024

/* Longest the thread sleeps at once, so that a stop is heard promptly. */
#define TONES_POLL_MS 10

/*
 * The tone playing. The thread clears playing when the tone ends, on its own
 * or stopped, and is joined by whoever starts or stops a tone next.
 */
static struct rtttl_note notes[RTTTL_MAX_NOTES];
static size_t count;
static pathId path;
static volatile bool playing;
static volatile bool stopReq;
static bool threadLive;
static pthread_t thread;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * Tell whether the tone should go on: not stopped, and the speaker still ours.
 */
static bool goingOn(void)
{
    return (stopReq == false) && (audioPath_getStatus(path) == PATH_OPEN);
}

/**
 * Play the notes, each one timed from the end of the one before, so that the
 * time taken to switch the beeper does not add up over the tone.
 */
static void *toneFunc(void *arg)
{
    (void)arg;
    long long due = getTick();

    for (size_t i = 0; (i < count) && goingOn(); i++) {
        if (notes[i].freq_hz > 0)
            platform_beepStart(notes[i].freq_hz);

        due += notes[i].duration_ms;
        long long now = getTick();
        while ((now < due) && goingOn()) {
            long long left = due - now;
            sleepFor(0u,
                     (unsigned int)((left < TONES_POLL_MS) ? left :
                                                             TONES_POLL_MS));
            now = getTick();
        }

        if (notes[i].freq_hz > 0)
            platform_beepStop();
    }

    audioPath_release(path);
    playing = false;
    return NULL;
}

/**
 * Stop the thread of the last tone, if it is still to be joined. With the
 * mutex held.
 */
static void reap(void)
{
    if (threadLive) {
        stopReq = true;
        pthread_join(thread, NULL);
        threadLive = false;
    }

    playing = false;
}

/**
 * Tell whether a tone can start, nothing else playing, and reap the last one.
 * With the mutex held.
 */
static bool claim(void)
{
    if (playing || vp_beepBusy())
        return false;

    reap();
    return true;
}

/**
 * Take the speaker and start the thread on the notes in place. With the mutex
 * held.
 */
static bool start(void)
{
    path = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_BEEP);
    if (audioPath_getStatus(path) != PATH_OPEN) {
        audioPath_release(path);
        return false;
    }

    stopReq = false;
    playing = true;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
#if defined(_MIOSIX)
    pthread_attr_setstacksize(&attr, TONES_THREAD_STKSIZE);
    struct sched_param param;
    param.sched_priority = THREAD_PRIO_HIGH;
    pthread_attr_setschedparam(&attr, &param);
#endif

    if (pthread_create(&thread, &attr, toneFunc, NULL) != 0) {
        playing = false;
        audioPath_release(path);
        return false;
    }

    threadLive = true;
    return true;
}

bool tones_playRtttl(const char *rtttl)
{
    uint8_t parsed = 0;
    bool started = false;

    pthread_mutex_lock(&mutex);
    if (claim() && rtttl_parse(rtttl, notes, &parsed)) {
        count = parsed;
        started = start();
    }
    pthread_mutex_unlock(&mutex);

    return started;
}

bool tones_playNotes(const struct rtttl_note *tone, const size_t length)
{
    bool started = false;

    if ((length == 0) || (length > RTTTL_MAX_NOTES))
        return false;

    pthread_mutex_lock(&mutex);
    if (claim()) {
        memcpy(notes, tone, length * sizeof(notes[0]));
        count = length;
        started = start();
    }
    pthread_mutex_unlock(&mutex);

    return started;
}

void tones_stop(void)
{
    pthread_mutex_lock(&mutex);
    reap();
    pthread_mutex_unlock(&mutex);
}

bool tones_playing(void)
{
    return playing;
}
