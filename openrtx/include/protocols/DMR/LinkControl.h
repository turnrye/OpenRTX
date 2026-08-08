/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_LINKCONTROL_H
#define DMR_LINKCONTROL_H

#include <stdint.h>
#include <stdbool.h>
#include "protocols/DMR/Datatypes.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A parsed full link control PDU. The voice call fields are only meaningful
 * when the opcode names a voice call; the payload octets are preserved
 * exactly as received for every opcode, so one this layer does not handle
 * yet loses nothing on its way through.
 */
struct dmrLinkControl
{
    uint32_t dstId;             ///< Voice calls: destination address.
    uint32_t srcId;             ///< Voice calls: source address.
    uint8_t  flco;              ///< Opcode, enum dmrFlco for the known ones.
    uint8_t  featureSet;        ///< Feature set identifier octet.
    uint8_t  serviceOptions;    ///< Voice calls: service options octet.
    uint8_t  payload[DMR_LC_LENGTH - 2];    ///< Octets following the opcode
                                            ///  and feature set, as received.
};

/**
 * Build the full link control transmitted in a voice header or terminator
 * burst, from the perspective of the calling station.
 *
 * The layout is the opcode carrying the call type, then the feature set
 * identifier and the service options, then the destination and source
 * addresses, each three bytes big endian. Only the nine information octets
 * exist at this layer: the parity protecting the block on air is error
 * correction, and belongs to the modem carrying the burst.
 *
 * @param lc: destination buffer, DMR_LC_LENGTH bytes long.
 * @param groupCall: true for a group call, false for a private one.
 * @param dstId: destination address, a talkgroup for a group call.
 * @param srcId: source address, that is the transmitting station's DMR ID.
 */
void dmr_buildVoiceLc(uint8_t *lc, const bool groupCall, const uint32_t dstId,
                      const uint32_t srcId);

/**
 * Parse a full link control PDU into its typed form. Any opcode parses: the
 * voice call fields are filled for the group and private call opcodes, and
 * the payload octets are preserved for every opcode, known or not.
 *
 * @param out: destination for the parsed form.
 * @param lc: source PDU, DMR_LC_LENGTH bytes long.
 */
void dmr_parseLc(struct dmrLinkControl *out, const uint8_t *lc);

#ifdef __cplusplus
}
#endif

#endif /* DMR_LINKCONTROL_H */
