/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RTX_MODEM_STUB_H
#define RTX_MODEM_STUB_H

/**
 * Test double for the rtx frame port: records the frames pushed by the code
 * under test and hands out the frames scripted by the test.
 */

#include <deque>
#include <vector>
#include "rtx/rtx.h"

struct rtxModemStub {
    std::vector<struct modemFrame> pushed; ///< Frames pushed for TX
    std::deque<struct modemFrame> toPop;   ///< Frames to hand out as RX
    int pushResult = 0;                    ///< Return value of push
    size_t txSpace = 4;                    ///< Reported TX space
    bool squelchOpen = false;              ///< Reported squelch state
    rtxStatus_t status = {};               ///< Reported rtx status
};

extern struct rtxModemStub rtxStub;

void rtxStub_reset();

#endif /* RTX_MODEM_STUB_H */
