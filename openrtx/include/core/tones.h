/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TONES_H
#define TONES_H

#include <stdbool.h>
#include <stddef.h>
#include "core/rtttl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Tones played on the speaker with the radio's beep generator, note by note: a
 * ringtone given in RTTTL, or notes given directly. A tone plays on a thread of
 * its own, which sleeps from one note to the next, so that its rhythm holds to
 * the millisecond whichever thread started it and whatever that thread does
 * meanwhile. A tone does not start over another tone, nor over a beep of the
 * user interface, and the user interface does not beep over a tone.
 */

/**
 * Start playing a tone given in RTTTL.
 *
 * @param rtttl: the tone, as a NUL-terminated RTTTL string.
 * @return true if the tone started, false if another tone or a beep is
 * playing, the string holds no notes or the speaker is taken.
 */
bool tones_playRtttl(const char *rtttl);

/**
 * Start playing a tone given as notes, a frequency of 0 being a pause.
 *
 * @param tone: the notes.
 * @param length: how many, at most RTTTL_MAX_NOTES.
 * @return true if the tone started, false if another tone or a beep is
 * playing, there are no notes or the speaker is taken.
 */
bool tones_playNotes(const struct rtttl_note *tone, const size_t length);

/**
 * Stop the tone playing, if any, and wait for it to be silent.
 */
void tones_stop(void);

/**
 * @return true while a tone plays.
 */
bool tones_playing(void);

#ifdef __cplusplus
}
#endif

#endif /* TONES_H */
