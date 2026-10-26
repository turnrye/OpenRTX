/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rtx_modem_stub.h"
#include "core/state.h"
#include "interfaces/platform.h"
#include <cerrno>

struct rtxModemStub rtxStub;

/* Globals normally provided by state.c and the platform layer. */
state_t state;
pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

static const hwInfo_t hwInfo = {
    .name = "Test",
    .hw_version = 1,
    .flags = 0,
    .uhf_band = 1,
    .vhf_band = 1,
    .uhf_maxFreq = 480,
    .uhf_minFreq = 400,
    .vhf_maxFreq = 174,
    .vhf_minFreq = 136,
};

void rtxStub_reset()
{
    rtxStub = rtxModemStub();
}

const hwInfo_t *platform_getHwInfo()
{
    return &hwInfo;
}

int rtx_modemPushTxFrame(const struct modemFrame *frame)
{
    if (rtxStub.pushResult != 0)
        return rtxStub.pushResult;

    rtxStub.pushed.push_back(*frame);
    return 0;
}

int rtx_modemPopRxFrame(struct modemFrame *frame)
{
    if (rtxStub.toPop.empty())
        return -EAGAIN;

    *frame = rtxStub.toPop.front();
    rtxStub.toPop.pop_front();
    return 0;
}

size_t rtx_modemTxSpace(void)
{
    return rtxStub.txSpace;
}

const rtxStatus_t *rtx_getStatus()
{
    return &rtxStub.status;
}

bool rtx_rxSquelchOpen()
{
    return rtxStub.squelchOpen;
}
