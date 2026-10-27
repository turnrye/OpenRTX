/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef FILE_SINK_H
#define FILE_SINK_H

#include "interfaces/audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Output audio driver appending the samples to a raw file, the file path
 * being the device configuration. It keeps the timing of a real output
 * peripheral: in circular double buffered mode each half buffer takes its
 * play time to go out, and sync() returns once the previous one has.
 */
extern const struct audioDriver file_sink_audio_driver;

#ifdef __cplusplus
}
#endif

#endif /* FILE_SINK_H */
