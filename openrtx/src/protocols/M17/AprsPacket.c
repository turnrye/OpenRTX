/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/M17/AprsPacket.h"
#include "protocols/M17/m17.h"
#include "protocols/APRS/packet.h"

size_t m17_aprsFormat(const uint8_t *frame, size_t len, uint8_t *buf,
                      size_t bufSize)
{
    if ((frame == NULL) || (buf == NULL) || (bufSize < 3))
        return 0;

    /* The line and its NUL after the identifier */
    size_t n = aprsFrameToTnc2(frame, len, (char *)&buf[1], bufSize - 1);
    if (n == 0)
        return 0;

    /* Identifier, line, NUL, and the CRC the framer appends */
    if (n + 4 > M17_MAX_PACKET_DATA)
        return 0;

    buf[0] = M17_PACKET_APRS;
    return n + 2;
}

size_t m17_aprsFrame(const uint8_t *data, size_t len, uint8_t *frame,
                     size_t frameSize)
{
    if ((data == NULL) || (frame == NULL) || (len < 2)
        || (data[0] != M17_PACKET_APRS))
        return 0;

    const char *line = (const char *)&data[1];
    size_t n = len - 1;
    while ((n > 0) && (line[n - 1] == '\0'))
        n--;

    return aprsFrameFromTnc2(line, n, frame, frameSize);
}
