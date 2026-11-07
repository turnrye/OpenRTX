/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Minimal environment for the APRS source test: the global state the source
 * reads its callsign from, and a clock the test sets.
 */

extern "C" {
#include "core/state.h"
#include "interfaces/delays.h"
#include "core/packet_engine.h"
}

state_t state;
long long fakeTick = 0;

long long getTick()
{
    return fakeTick;
}

void packetEngine_send(const uint8_t *frame, size_t len)
{
    (void)frame;
    (void)len;
}
