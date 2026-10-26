/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MODEM_FRAME_H
#define MODEM_FRAME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Raw modem frame exchanged between a host computer and the radio when the
 * radio acts as a host-driven modem: the host does the whole data link layer
 * and the radio only modulates and demodulates physical layer frames.
 *
 * The payload is the frame exactly as it goes on air after channel coding
 * (for M17: sync word plus 46 bytes of type-4 bits). The frame type is a
 * protocol-specific small integer; each protocol defines its own values.
 * Marker frames (sync lost, end of transmission) carry no payload.
 */

#define MODEM_FRAME_MAX_LEN 48

enum modemFrameFlags {
    MODEM_FRAME_FIRST = 0x01, /**< First frame after (re)acquiring sync */
};

struct modemFrame {
    uint8_t type;  /**< Protocol-specific frame type       */
    uint8_t flags; /**< Bitmask of modemFrameFlags         */
    uint8_t len;   /**< Payload length, zero for a marker  */
    uint8_t data[MODEM_FRAME_MAX_LEN]; /**< Frame payload  */
};

#ifdef __cplusplus
}
#endif

#endif /* MODEM_FRAME_H */
