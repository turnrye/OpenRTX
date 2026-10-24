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
#define DMR_LC_LENGTH 12

/* Full link control opcodes distinguishing a group call from a private one. */
#define DMR_FLCO_GROUP 0x00
#define DMR_FLCO_PRIVATE 0x03

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

/**
 * Length of a control signalling block, CSBK, as handed to the chip: the 80-bit
 * block without its CRC, which the chip adds.
 */
#define DMR_CSBK_LENGTH 10

/* CSBK opcode asking a repeater to start its downlink, BS_Dwn_Act. */
#define DMR_CSBKO_BS_DWN_ACT 0x38

/**
 * Build the control signalling block that wakes a repeater up, BS_Dwn_Act, as
 * the CS7000 Plus stock firmware sends it: the last block, the standard feature
 * set, addressed to every repeater, from the calling station.
 *
 * @param csbk: destination buffer, DMR_CSBK_LENGTH bytes long.
 * @param srcId: the transmitting station's DMR ID.
 */
void dmr_buildWakeupCsbk(uint8_t *csbk, const uint32_t srcId);

/**
 * A voice call, as its full link control describes it.
 */
struct dmrVoiceCall {
    uint32_t dstId; ///< Talkgroup, or the called station for a private call.
    uint32_t srcId; ///< Calling station.
    bool groupCall; ///< Group call, or private.
    bool encrypted; ///< Privacy set in the service options.
};

/**
 * Read the voice call a full link control describes, as received from a voice
 * header, a terminator or the embedded signalling of a superframe.
 *
 * @param lc: full link control, at least 9 bytes.
 * @param call: destination for the call.
 * @return true for the link control of a group or private voice call, false
 * for any other kind, leaving the call untouched.
 */
bool dmr_parseVoiceLc(const uint8_t *lc, struct dmrVoiceCall *call);

/**
 * Get the opcode of a full link control.
 *
 * @param lc: full link control.
 * @return the full link control opcode, FLCO.
 */
static inline uint8_t dmr_lcOpcode(const uint8_t *lc)
{
    return lc[0] & 0x3F;
}

#ifdef __cplusplus
}
#endif

#endif /* DMR_LINKCONTROL_H */
