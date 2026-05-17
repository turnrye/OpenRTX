/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef M17_SMS_H
#define M17_SMS_H

#ifdef CONFIG_M17

#include "core/messages.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise the M17 SMS source.  Called once from rtx_threadFunc before
 * the RTX loop starts.  Resets the entry pool and registers a permanent
 * RX packet descriptor with the RTX subsystem.
 */
void m17_sms_init(void);

/**
 * Poll TX/RX packet descriptor status.  Called from rtx_threadFunc every
 * RTX loop iteration.  Promotes completed/failed TX descriptors to their
 * final status and ingests completed RX descriptors as new inbox entries.
 */
void m17_sms_task(void);

/**
 * Enqueue an outgoing SMS.
 *
 * Creates a new inbox entry with MSG_STATUS_SENDING, formats the M17 SMS
 * application-layer packet, and submits it via rtx_addPacketTx().
 *
 * @param message:   NUL-terminated message text.
 * @param msgLen:    Length of @p message not counting the NUL.
 * @param recipient: Destination callsign (up to 9 chars + NUL).
 * @return 0 on success, -EBUSY if a TX is already in flight,
 *         -EINVAL for bad arguments, -EMSGSIZE if text is too large.
 */
int m17_sms_send(const char *message, size_t msgLen, const char *recipient);

/** Vtable instance for the M17 SMS source. Referenced by messages.cpp. */
extern const message_type_vtable_t m17_sms_vtable;

/** True when a compose session has been requested but not yet dismissed. */
bool m17_sms_compose_pending(void);

/**
 * Recipient pre-filled from a REPLY action.
 * Returns an empty string when compose was started without a pre-fill.
 */
const char *m17_sms_compose_recipient(void);

/** Clear the compose-pending flag (call after compose screen is dismissed). */
void m17_sms_compose_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_M17 */
#endif /* M17_SMS_H */
