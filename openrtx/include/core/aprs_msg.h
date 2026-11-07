/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APRS_MSG_H
#define APRS_MSG_H

#include "core/messages.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Message source for APRS addressed text messages.
 *
 * Registered with the inbox on targets that define CONFIG_APRS. Received
 * frames arrive through the inbox's packet descriptors, sent messages leave
 * the same way; this source only translates between AX.25 frames and
 * inbox entries.
 */
extern const struct messageOps aprs_msg_ops;

/**
 * Forget the acknowledgement bookkeeping: messages heard, acks owed and our
 * messages waiting for one.
 */
void aprs_msg_reset(void);

/**
 * The emergency most recently heard and not yet acknowledged by the
 * operator, if any. Every emergency packet raises it again unless the same
 * station's was acknowledged within the last ten minutes.
 *
 * @param call: filled with the station raising it.
 * @param len: size of call.
 * @param since: filled with when it was heard, seconds of system tick.
 * @return true if an emergency is pending.
 */
bool aprs_msg_emergency(char *call, size_t len, uint32_t *since);

/** The operator has seen the pending emergency. */
void aprs_msg_emergencyAck(void);

#ifdef __cplusplus
}
#endif

#endif /* APRS_MSG_H */
