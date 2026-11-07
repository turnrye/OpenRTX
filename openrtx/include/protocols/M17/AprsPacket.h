/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef M17_APRSPACKET_H
#define M17_APRSPACKET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The M17 specification reserves packet identifier 0x02 for APRS without
 * defining its payload. Here it is a TNC2 monitor line, "SRC>DEST,PATH:info",
 * followed by a NUL, in the style of the specified SMS (0x05) and TLE (0x07)
 * payloads, so that a gateway can pass it to and from APRS-IS nearly as is.
 * 0x01 (AX.25) is left to conventional packet radio.
 */
#define M17_PACKET_APRS 0x02

/**
 * Write an AX.25 frame as an M17 APRS packet payload: [0x02][line][0x00].
 *
 * @param frame: AX.25 UI frame without its FCS.
 * @param len: frame length.
 * @param buf: output buffer.
 * @param bufSize: size of the output buffer.
 * @return payload length, or 0 if the frame is malformed or the payload does
 *         not fit in the buffer or in one M17 packet.
 */
size_t m17_aprsFormat(const uint8_t *frame, size_t len, uint8_t *buf,
                      size_t bufSize);

/**
 * Get the AX.25 frame an M17 APRS packet payload describes.
 *
 * @param data: reassembled packet data, starting with the identifier.
 * @param len: its length, without the CRC.
 * @param frame: buffer to receive the AX.25 frame.
 * @param frameSize: its size.
 * @return frame length, or 0 if the payload is not APRS, or its line is
 *         malformed or names an address AX.25 cannot carry.
 */
size_t m17_aprsFrame(const uint8_t *data, size_t len, uint8_t *frame,
                     size_t frameSize);

#ifdef __cplusplus
}
#endif

#endif /* M17_APRSPACKET_H */
