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
 * Driver providing an audio output stream to a file. File format is raw,
 * signed 16-bit, little endian (S16LE). The configuration parameter is the
 * file name with the full path.
 */

extern const struct audioDriver file_sink_audio_driver;

#ifdef __cplusplus
}
#endif

#endif /* FILE_SINK_H */
