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
/*
 * The rtx thread formats diagnostic messages, and the integer only printf of
 * the C library alone takes some 200 bytes of stack to do so. 512 bytes were
 * enough as long as no operating mode did it, but they overflow as soon as one
 * does.
 */
#define RTX_THREAD_STKSIZE    1024
#define CODEC2_THREAD_STKSIZE 16384
#define AUDIO_THREAD_STKSIZE  512

/**
 * Thread priority levels, UNIX-like: lower level, higher thread priority
 */
#ifdef _MIOSIX
#define THREAD_PRIO_RT      0
#define THREAD_PRIO_HIGH    1
#define THREAD_PRIO_NORMAL  2
#define THREAD_PRIO_LOW     3
#endif

/**
 * Spawn all the threads for the various functionalities.
 */
void create_threads();

#endif /* THREADS_H */
