/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_DEBUG_H
#define DMR_DEBUG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Host-side debug tap for DMR bring-up.
 *
 * Messages are buffered in RAM by the producing thread and flushed to the host
 * link by dmrDebug_task(), which has to be called periodically from a thread
 * tolerant to blocking I/O. On radio targets the link is the USB CDC device
 * already brought up at boot, on the linux target it is standard output.
 *
 * The output is line oriented ASCII, so that it can be read directly with a
 * terminal emulator and, at the same time, parsed by the host side tools:
 *
 *   [12345] some free form message
 *   $VP 12345 A1B2C3... 5F
 *
 * Log lines start with the millisecond timestamp of the tick they were emitted
 * in, binary records start with a '$' followed by a record tag, the timestamp,
 * the payload as uppercase hex and a modulo-256 checksum of the payload bytes.
 *
 * NOTE: none of these functions is safe to call from an interrupt handler, as
 * the message buffer is guarded by a mutex. Interrupt handlers have to limit
 * themselves to updating counters, leaving the logging to the thread draining
 * them.
 */

/**
 * Debug tap message categories, used to keep the high rate records from
 * drowning out the low rate ones. Categories are enabled independently.
 */
enum dmrDebugChannel {
    DMR_DBG_INFO = 0x01, ///< Mode changes, init results, errors.
    DMR_DBG_IRQ = 0x02,  ///< Periodic interrupt counters.
    DMR_DBG_LC = 0x04,   ///< Decoded link control.
    DMR_DBG_VP = 0x08    ///< Raw voice payload records.
};

/**
 * Initialise the debug tap.
 */
void dmrDebug_init();

/**
 * Shut down the debug tap, discarding any message still buffered.
 */
void dmrDebug_terminate();

/**
 * Flush buffered messages towards the host and handle incoming commands.
 * Has to be called periodically from a thread which can tolerate blocking on
 * the host link.
 */
void dmrDebug_task();

/**
 * Enable or disable a set of message categories.
 *
 * @param channels: bitmap of dmrDebugChannel values to act upon.
 * @param enable: true to enable the given categories, false to disable them.
 */
void dmrDebug_enableChannels(const uint8_t channels, const bool enable);

/**
 * Check whether a message category is enabled. Useful to skip the cost of
 * formatting a message which would be discarded anyway.
 *
 * @param channel: category to be tested.
 * @return true if the category is enabled.
 */
bool dmrDebug_channelEnabled(const enum dmrDebugChannel channel);

/**
 * Emit a printf-style log line.
 *
 * @param channel: category the message belongs to.
 * @param fmt: printf-style format string.
 */
void dmrDebug_log(const enum dmrDebugChannel channel, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/**
 * Write a marker straight to the host link, bypassing the buffer.
 *
 * Ordinary messages are only handed over when the thread draining the buffer
 * next runs, so nothing written from a step which then locks the radio up ever
 * reaches the host. This blocks until the marker is out, which makes it usable
 * to find out how far a bring-up got before it stopped, and unsuitable for
 * anything else.
 *
 * @param msg: text of the marker.
 */
void dmrDebug_mark(const char *msg);

/**
 * Emit a binary record, hex encoded and checksummed.
 *
 * @param channel: category the record belongs to.
 * @param tag: two-character record tag.
 * @param data: record payload.
 * @param len: payload length, in bytes.
 */
void dmrDebug_record(const enum dmrDebugChannel channel, const char *tag,
                     const void *data, const size_t len);

/**
 * A command run from the host link.
 */
typedef void (*dmrDebugCommand)(void);

/**
 * Run a command when its key comes from the host, besides the channel
 * toggles '1' to '4' and '?' for the status. Drivers register theirs while
 * they are running.
 *
 * @param key: key of the command.
 * @param command: command to run, NULL to remove the key.
 * @param help: few words on the command, for '?'.
 * @return 0 on success, -1 if no more commands fit.
 */
int dmrDebug_setCommand(const char key, dmrDebugCommand command,
                        const char *help);

#ifdef __cplusplus
}
#endif

#endif /* DMR_DEBUG_H */
