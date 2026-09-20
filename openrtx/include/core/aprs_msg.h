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

#ifdef __cplusplus
}
#endif

#endif /* APRS_MSG_H */
