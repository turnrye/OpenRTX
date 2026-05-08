/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SMS_H
#define SMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise the SMS manager. Must be called once at startup before any
 * other sms_* function.
 */
void sms_init(void);

/**
 * Tick function — call periodically from the main loop (same cadence as
 * ui_updateFSM). Polls pending RX descriptors; on completion calls
 * sms_parse_packet and pushes decoded messages into the internal queue.
 * Also clears the TX-pending flag when a TX descriptor completes.
 */
void sms_task(void);

/**
 * Return the number of received messages waiting in the queue.
 */
uint8_t sms_count(void);

/**
 * Retrieve a message by index. Returns false if index is out of range.
 *
 * @param index:      zero-based message index.
 * @param sender:     buffer to receive the sender callsign.
 * @param sender_len: size of the sender buffer in bytes.
 * @param message:    buffer to receive the message text.
 * @param message_len: size of the message buffer in bytes.
 * @return true if the index was valid and buffers were filled.
 */
bool sms_get(uint8_t index, char *sender, size_t sender_len, char *message,
             size_t message_len);

/**
 * Delete a message by index.
 *
 * @param index: zero-based position to remove.
 */
void sms_erase(uint8_t index);

/**
 * Enqueue an SMS for transmission. The text is formatted with
 * sms_format_packet and submitted via rtx_addPacketTx.
 *
 * @param message: null-terminated message text to send.
 * @param msgLen:  length of message not counting the NUL terminator.
 * @return 0 on success, -EBUSY if a TX is already in progress,
 *         -EINVAL if the message is empty.
 */
int sms_send(const char *message, size_t msgLen);

/**
 * Returns true if a TX initiated by sms_send is still in progress.
 */
bool sms_tx_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* SMS_H */
