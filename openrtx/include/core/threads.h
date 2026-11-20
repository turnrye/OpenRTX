/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef THREADS_H
#define THREADS_H

#include <stddef.h>

/**
 * Threads' stack sizes
 */
#define UI_THREAD_STKSIZE     2048
#define RTX_THREAD_STKSIZE    512
#define CODEC2_THREAD_STKSIZE 16384
#define AUDIO_THREAD_STKSIZE  512
#define USB_THREAD_STKSIZE    1024
#define USB_THREAD_STKSIZE    1024

/**
 * Thread priority levels, UNIX-like: lower level, higher thread priority
 */
#ifdef _MIOSIX
#define THREAD_PRIO_RT      0
#define THREAD_PRIO_HIGH    1
#define THREAD_PRIO_NORMAL  2
#define THREAD_PRIO_LOW     3
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Spawn all the threads for the various functionalities.
 */
void create_threads();

/**
 * Have the UI thread apply the radio state to the rtx module, as after a
 * keypress, for a change made outside the UI such as over CAT. Call with
 * state_mutex held.
 */
void threads_requestRtxSync();

#ifdef __cplusplus
}
#endif

#endif /* THREADS_H */
