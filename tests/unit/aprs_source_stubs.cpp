/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Minimal environment for the APRS source test: the global state the source
 * reads its callsign from, and a no-op KISS emitter so the receive path does
 * not drag the console engine into the test.
 */

extern "C" {
#include "core/state.h"
#include "core/packet_engine.h"
}

state_t state;

void packetEngine_send(const uint8_t *frame, size_t len)
{
    (void)frame;
    (void)len;
}
