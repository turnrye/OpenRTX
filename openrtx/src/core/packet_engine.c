/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <unistd.h>
#include <string.h>
#include "protocols/APRS/constants.h"
#include "core/packet_engine.h"
#include "core/slip.h"

/*
 * One KISS data frame: the command byte (port 0, data) followed by the raw
 * AX.25 frame. Static rather than allocated: this runs on whichever thread
 * handed the frame over, and never more than one frame at a time.
 */
static uint8_t kissBuf[APRS_PACLEN + 1];

void packetEngine_send(const uint8_t *frame, size_t len)
{
    uint8_t txBuf[256];
    struct slip kissFrame;
    int ret;

    if ((frame == NULL) || (len == 0) || (len > APRS_PACLEN))
        return;

    kissBuf[0] = 0;
    memcpy(&kissBuf[1], frame, len);

    slip_init(&kissFrame, kissBuf, len + 1);

    while ((ret = slip_encode(&kissFrame, txBuf, sizeof(txBuf))) > 0)
        write(STDOUT_FILENO, txBuf, ret);
}
