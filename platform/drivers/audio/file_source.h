/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef FILE_SOURCE_H
#define FILE_SOURCE_H

#include <stdbool.h>
#include "interfaces/audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Configuration for the file source driver.
 *
 * The file path is taken from the environment variable named by envVar when
 * it is set and non-empty, otherwise defaultPath is used. When loop is true the
 * file is replayed from the beginning once its end is reached; when false the
 * stream continues with silence (zero samples) after the end of the file.
 */
struct fileSourceConfig {
    const char *envVar;
    const char *defaultPath;
    bool loop;
};

/**
 * Driver providing an audio input stream from a file. File format should be
 * raw, 16 bit, signed, little endian, mono, at the stream sample rate. The
 * configuration parameter is a pointer to a struct fileSourceConfig.
 */
extern const struct audioDriver file_source_audio_driver;

/**
 * Resolve the file path described by a fileSourceConfig.
 *
 * @param cfg: file source configuration.
 * @return the path from the environment variable if set and non-empty,
 * otherwise the default path.
 */
const char *fileSource_path(const struct fileSourceConfig *cfg);

#ifdef __cplusplus
}
#endif

#endif /* FILE_SOURCE_H */
