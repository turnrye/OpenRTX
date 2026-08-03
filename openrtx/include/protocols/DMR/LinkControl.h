/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_LINKCONTROL_H
#define DMR_LINKCONTROL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Length of a DMR full link control block, in bytes. The chip is handed the
 * plain link control and adds the error correction and interleaving itself, so
 * this is the bare 72-bit block with no forward error correction.
 */
#define DMR_LC_LENGTH   12

/* Full link control opcodes distinguishing a group call from a private one. */
#define DMR_FLCO_GROUP      0x00
#define DMR_FLCO_PRIVATE    0x03

/**
 * Build the full link control transmitted in a voice header or terminator
 * burst, from the perspective of the calling station.
 *
 * The layout matches what the receiver decodes back from the chip: the first
 * byte carries the call type, then the feature set identifier and the service
 * options, then the destination and source addresses, each three bytes big
 * endian. The remaining bytes, which would carry the forward error correction
 * on air, are left zero for the chip to fill in.
 *
 * @param lc: destination buffer, DMR_LC_LENGTH bytes long.
 * @param groupCall: true for a group call, false for a private one.
 * @param dstId: destination address, a talkgroup for a group call.
 * @param srcId: source address, that is the transmitting station's DMR ID.
 */
void dmr_buildVoiceLc(uint8_t *lc, const bool groupCall, const uint32_t dstId,
                      const uint32_t srcId);

#ifdef __cplusplus
}
#endif

#endif /* DMR_LINKCONTROL_H */
