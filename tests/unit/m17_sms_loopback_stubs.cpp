/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * Loopback stubs for m17_sms_loopback_test.
 *
 * Key difference from m17_sms_stubs.cpp: rtx_addPacketRx() saves the
 * pktDesc pointer so the test can write a formatted SMS into the live
 * RX buffer and mark the descriptor PKT_STATUS_DONE.
 */

#include <cstring>

extern "C" {
#include "rtx/rtx.h"
#include "interfaces/platform.h"
}

static struct pktDesc *g_rx_desc = nullptr;
static struct pktDesc *g_tx_desc = nullptr;

extern "C" {

/**
 * Return the last pktDesc pointer registered via rtx_addPacketRx().
 * Valid after the first m17_sms_task_rtx() call following m17_sms_init().
 */
struct pktDesc *loopback_get_rx_desc(void)
{
    return g_rx_desc;
}

/**
 * Return the last pktDesc pointer passed to rtx_addPacketTx().
 * Valid after the first m17_sms_send() call.
 */
struct pktDesc *loopback_get_tx_desc(void)
{
    return g_tx_desc;
}

int rtx_addPacketRx(struct pktDesc *pkt)
{
    g_rx_desc = pkt;
    if (pkt != nullptr)
        pkt->status = PKT_STATUS_IDLE;
    return 0;
}

int rtx_addPacketTx(struct pktDesc *pkt)
{
    g_tx_desc = pkt;
    if (pkt != nullptr)
        pkt->status = PKT_STATUS_DONE;
    return 0;
}

rtxStatus_t rtx_getCurrentStatus(void)
{
    rtxStatus_t s;
    memset(&s, 0, sizeof(s));
    strncpy(s.M17_src, "W1AW", sizeof(s.M17_src) - 1);
    return s;
}

datetime_t platform_getCurrentTime(void)
{
    datetime_t t;
    memset(&t, 0, sizeof(t));
    return t;
}

} /* extern "C" */
