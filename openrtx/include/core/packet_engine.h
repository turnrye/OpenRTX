/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PACKET_ENGINE_H
#define PACKET_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Emit one received AX.25 frame to the host as a SLIP-framed KISS data frame
 * on the console.
 *
 * The engine no longer owns receive descriptors: the message inbox is the
 * single consumer of decoded frames, and hands each one here so a host
 * listening on the console sees the same traffic the radio does.
 *
 * @param frame: raw AX.25 frame, without its frame check sequence.
 * @param len: frame length in bytes.
 */
void packetEngine_send(const uint8_t *frame, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* PACKET_ENGINE_H */
