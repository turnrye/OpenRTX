/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_DATATYPES_H
#define DMR_DATATYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Data link and call control types of the DMR air interface, as defined by
 * ETSI TS 102 361-1 and -2, in the shapes OpenRTX moves them around in.
 * Everything in this header is a fact of the specification or an OpenRTX
 * decision: nothing is specific to any baseband chip, and everything above
 * the baseband driver trades exclusively in these types.
 */

/**
 * Length of a full link control PDU: the nine information octets carrying the
 * opcode, the feature set identifier, the service options and the destination
 * and source addresses. The parity octets protecting a link control on air
 * are layer 2 error correction and never cross this layer's boundary: the
 * modem carrying the burst owns them.
 */
#define DMR_LC_LENGTH 9

/** Voice payload of one burst: three 9-byte AMBE+2 frames. */
#define DMR_PAYLOAD_LENGTH 27

/** Voice frames in a voice superframe; the burst sequence cycles 0 to 5. */
#define DMR_SUPERFRAME_FRAMES 6

/**
 * Colour code standing for "not observed". Every burst carries a colour code
 * on air, but not every modem can report it: the value a burst or a call
 * holds is what the modem observed, not necessarily what was sent.
 */
#define DMR_CC_UNKNOWN 0xFF

/** Destination address of an all call. Addresses are 24 bits on air. */
#define DMR_ADDR_ALL 0xFFFFFFu

/** Logical channels per RF channel: DMR is two-slot TDMA. */
#define DMR_SLOT_COUNT 2

/**
 * Data type of a data burst, from the slot type PDU. Voice bursts sit outside
 * this table: burst A of a superframe carries a sync pattern and bursts B to
 * F carry embedded signalling, so a voice burst is identified by its position
 * in the superframe rather than by a data type.
 */
enum dmrDataType {
    DMR_DATA_PI_HEADER = 0x00,        ///< Voice privacy indicator header.
    DMR_DATA_VOICE_HEADER = 0x01,     ///< Voice link control header.
    DMR_DATA_TERMINATOR = 0x02,       ///< Terminator with link control.
    DMR_DATA_CSBK = 0x03,             ///< Control signalling block.
    DMR_DATA_MBC_HEADER = 0x04,       ///< Multi block control, header.
    DMR_DATA_MBC_CONTINUATION = 0x05, ///< Multi block control, follow-on.
    DMR_DATA_HEADER = 0x06,           ///< Data header.
    DMR_DATA_RATE12 = 0x07,           ///< Rate 1/2 data.
    DMR_DATA_RATE34 = 0x08,           ///< Rate 3/4 data.
    DMR_DATA_IDLE = 0x09,             ///< Idle.
    DMR_DATA_RATE1 = 0x0A,            ///< Rate 1 data.
    DMR_DATA_UNKNOWN = 0xFF,          ///< The modem could not classify the
                                      ///  burst; not an on-air value.
};

/**
 * Full link control opcode, the first octet of a link control PDU. Opcodes
 * not listed here still exist on air; a parser hands them over unharmed
 * rather than dropping them.
 */
enum dmrFlco {
    DMR_FLCO_GROUP = 0x00,     ///< Group voice channel user.
    DMR_FLCO_PRIVATE = 0x03,   ///< Unit to unit voice channel user.
    DMR_FLCO_TA_HEADER = 0x04, ///< Talker alias, header block.
    DMR_FLCO_TA_BLOCK1 = 0x05, ///< Talker alias, continuation block 1.
    DMR_FLCO_TA_BLOCK2 = 0x06, ///< Talker alias, continuation block 2.
    DMR_FLCO_TA_BLOCK3 = 0x07, ///< Talker alias, continuation block 3.
    DMR_FLCO_GPS_INFO = 0x08,  ///< GPS position of the talker.
};

/**
 * The type of a call, mirroring the codeplug's contact types so that channel
 * wiring later is a change of data source rather than of vocabulary. An all
 * call is its own case, not "not a group call": it is addressed to
 * DMR_ADDR_ALL and every receiver takes it.
 */
enum dmrCallType {
    DMR_CALL_GROUP = 0,   ///< Group call, to a talkgroup.
    DMR_CALL_PRIVATE = 1, ///< Private call, to a single radio.
    DMR_CALL_ALL = 2,     ///< Broadcast to everyone on the channel.
};

/**
 * Sync pattern class of a received burst. A base station sync means the far
 * end is a repeater; the two direct mode values name the timeslot the
 * transmission calls itself, which is the only place a timeslot number
 * appears on air in direct mode.
 */
enum dmrSyncClass {
    DMR_SYNC_MS = 0,      ///< Mobile station sourced.
    DMR_SYNC_BS = 1,      ///< Base station sourced.
    DMR_SYNC_TS1 = 2,     ///< Direct mode, timeslot 1.
    DMR_SYNC_TS2 = 3,     ///< Direct mode, timeslot 2.
    DMR_SYNC_UNKNOWN = 4, ///< The modem did not observe the sync pattern.
};

/**
 * One received burst, as a modem hands it to the protocol stack. This is the
 * boundary the hardware draws: everything a baseband chip does is take in and
 * give out bursts, so it is the one seam every roadmap feature crosses
 * unchanged.
 *
 * A modem delivers every frame event as a burst, including corrupted ones:
 * the call state machine's liveness and terminator rules feed on events which
 * fail their checks, so a modem which filtered to clean bursts would silently
 * break them. Fields a modem cannot fill carry their "unknown" value; the
 * consumer filters.
 */
struct dmrBurst {
    uint8_t slot;        ///< Logical channel the burst arrived on, 0 or 1.
    bool voice;          ///< Voice burst rather than a data burst.
    uint8_t dataType;    ///< enum dmrDataType, or the voice sequence.
    uint8_t syncClass;   ///< enum dmrSyncClass observed for the burst.
    uint8_t colourCode;  ///< Observed colour code, or DMR_CC_UNKNOWN.
    bool crcOk;          ///< The burst's own error check passed.
    bool privacy;        ///< The payload is encrypted.
    uint8_t voiceFrames; ///< Payload frames holding decodable voice, 0 to 3.
    bool lcValid;        ///< A link control was decoded for this burst.
    uint8_t lc[DMR_LC_LENGTH];           ///< Link control, when lcValid.
    uint8_t payload[DMR_PAYLOAD_LENGTH]; ///< Voice payload of the burst.
};

/**
 * One burst queued for transmission: what to send, with the slot timing left
 * to the modem. Voice payloads are already protected by the vocoder's forward
 * error correction; the link control parity is the modem's to add.
 */
struct dmrTxBurst {
    bool voice;                ///< Voice burst rather than a data burst.
    uint8_t dataType;          ///< enum dmrDataType, or the voice sequence.
    uint8_t lc[DMR_LC_LENGTH]; ///< Link control to carry.
    uint8_t payload[DMR_PAYLOAD_LENGTH]; ///< Voice payload to carry.
};

/**
 * A call as the call control layer tracks it, and as the user interface
 * shows it.
 */
struct dmrCall {
    uint32_t srcId;     ///< DMR ID of the station calling.
    uint32_t dstId;     ///< Talkgroup or radio the call is addressed to.
    uint8_t callType;   ///< enum dmrCallType of the call.
    uint8_t colourCode; ///< Observed colour code, or DMR_CC_UNKNOWN.
    bool valid;         ///< The link control has been confirmed.
    bool voiceActive;   ///< Decodable voice is arriving for this call.
};

#ifdef __cplusplus
}
#endif

#endif /* DMR_DATATYPES_H */
