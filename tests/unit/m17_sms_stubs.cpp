/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * Minimal stubs for rtx and platform functions needed by m17_sms.cpp when
 * built outside the full firmware binary.  Used only by m17_sms_source_test.
 */

#include <cstring>
#include "rtx/rtx.h"
#include "interfaces/platform.h"

extern "C" {

int rtx_addPacketRx(struct pktDesc *pkt)
{
    if(pkt != nullptr)
        pkt->status = PKT_STATUS_IDLE;
    return 0;
}

int rtx_addPacketTx(struct pktDesc *pkt)
{
    /* Simulate instant TX completion so subsequent sends are not blocked. */
    if(pkt != nullptr)
        pkt->status = PKT_STATUS_DONE;
    return 0;
}

rtxStatus_t rtx_getCurrentStatus(void)
{
    rtxStatus_t s;
    memset(&s, 0, sizeof(s));
    return s;
}

datetime_t platform_getCurrentTime(void)
{
    datetime_t t;
    memset(&t, 0, sizeof(t));
    return t;
}

} /* extern "C" */
