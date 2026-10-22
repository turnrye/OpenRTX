/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_AUDIO_H
#define DMR_AUDIO_H

#include <stdbool.h>
#include <stdint.h>
#include "interfaces/dmr_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Received DMR voice playback. Voice bursts arrive in 60 ms groups while the
 * speaker DAC plays continuously, so decoded audio is buffered and paced by a
 * dedicated thread, mirroring the Codec2 decode path M17 uses. AMBE frames are
 * pushed as they are read off the chip; the thread decodes and plays them.
 */

/**
 * Open the speaker audio path and start the decode/playback thread. Needs the
 * vocoder blob installed.
 *
 * @return true if playback started, false if the codec is unavailable or the
 *         audio path could not be opened.
 */
bool dmrAudio_startRx(void);

/**
 * Stop playback and release the audio path. Idempotent.
 */
void dmrAudio_stopRx(void);

/**
 * @return true while the playback thread is running.
 */
bool dmrAudio_running(void);

/**
 * Queue one 9-byte AMBE voice frame for playback. Dropped if playback is not
 * running or the queue is full.
 */
void dmrAudio_pushFrame(const uint8_t frame[DMR_CODEC_FRAME_BYTES]);

/**
 * Open the microphone audio path and start the capture/encode thread. Needs the
 * vocoder blob installed.
 *
 * @return true if capture started, false if the codec is unavailable or the
 *         audio path could not be opened.
 */
bool dmrAudio_startTx(void);

/**
 * Stop capture and release the microphone path. Idempotent.
 */
void dmrAudio_stopTx(void);

/**
 * @return true while the capture thread is running.
 */
bool dmrAudio_txRunning(void);

/**
 * Take the next encoded AMBE frame for transmission. Lock-free, for the single
 * consumer that the baseband is, and only while capture is running.
 *
 * @param frame: destination, DMR_CODEC_FRAME_BYTES (9) bytes.
 * @return true if a frame was returned, false if none is ready.
 */
bool dmrAudio_popFrame(uint8_t frame[DMR_CODEC_FRAME_BYTES]);

#ifdef __cplusplus
}
#endif

#endif /* DMR_AUDIO_H */
