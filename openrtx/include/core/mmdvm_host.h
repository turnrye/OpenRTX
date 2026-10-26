/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MMDVM_HOST_H
#define MMDVM_HOST_H

#include "interfaces/chardev.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * MMDVM modem protocol towards a host computer.
 *
 * The radio answers as an MMDVM-compatible modem (protocol version 1) over a
 * character device: the host configures frequency and mode, polls the status
 * and exchanges M17 frames with the host-driven modem operating mode through
 * the rtx frame port. Hosts known to work with this dialect are m17-gateway
 * (modem type "mmdvm") and M17Client.
 *
 * The functions are meant to be called from a single thread.
 */

/**
 * Initialise the protocol handler.
 *
 * @param dev: character device connected to the host.
 */
void mmdvmHost_init(const struct chardev *dev);

/**
 * Shut down the protocol handler.
 */
void mmdvmHost_terminate(void);

/**
 * Periodic task: parse incoming commands and forward received frames to the
 * host. To be called at least every 20 ms.
 */
void mmdvmHost_task(void);

/**
 * Check whether a host is talking to the radio.
 *
 * @return true if the host sent something recently.
 */
bool mmdvmHost_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* MMDVM_HOST_H */
