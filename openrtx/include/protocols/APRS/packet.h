/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APRS_PACKET_H
#define APRS_PACKET_H

#include "protocols/APRS/constants.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximum number of AX.25 addresses parsed out of one frame.
 *
 * An AX.25 frame carries a destination, a source, and up to eight digipeater
 * addresses. Frames with more are malformed and are rejected outright.
 */
#define APRS_MAX_ADDRESSES 10

/**
 * @brief Maximum length of a formatted "CALLSIGN-SSID" string, with NUL.
 *
 * Six callsign characters, a dash, two SSID digits, and the terminator.
 */
#define APRS_ADDR_STR_LEN 10

/**
 * @brief One AX.25 address: callsign, SSID, and the has-been-repeated bit.
 */
struct aprsAddress {
    char addr[7];         /**< Callsign, unshifted and NUL-terminated. */
    uint8_t ssid     : 4; /**< Secondary station identifier, 0-15.     */
    uint8_t repeated : 1; /**< H bit: this digipeater has repeated it. */
    uint8_t reserved : 3;
};

/**
 * @brief APRS data type, derived from the data type identifier (DTI).
 *
 * The DTI is the first byte of the info field and says how the rest of it
 * should be read. Only the classes the inbox distinguishes today are named;
 * everything else is APRS_TYPE_OTHER, which still displays as raw text. The
 * value is stored in struct message_header's type field.
 */
enum aprsType {
    APRS_TYPE_OTHER = 0, /**< Unrecognised or unclassified DTI.  */
    APRS_TYPE_MESSAGE,   /**< ':' addressed text message.        */
    APRS_TYPE_POSITION,  /**< '!' '=' '/' '@' position report.   */
    APRS_TYPE_STATUS,    /**< '>' status text.                   */
    APRS_TYPE_OBJECT,    /**< ';' object, ')' item.              */
    APRS_TYPE_TELEMETRY, /**< 'T' telemetry report.              */
};

/**
 * @brief A parsed APRS packet.
 *
 * Storage is supplied by the caller and everything is inline: no heap, no
 * pointers into the frame it came from, and no list links. Callers that want
 * to keep packets around own the array they live in and decide themselves
 * when a slot is reused.
 */
struct aprsPacket {
    struct aprsAddress addresses[APRS_MAX_ADDRESSES];
    uint8_t addressesLen;   /**< Addresses actually present.        */
    uint8_t infoLen;        /**< Info bytes, not counting the NUL.  */
    enum aprsType type;     /**< Classification of info[0].         */
    char info[APRS_PACLEN]; /**< Info field, NUL-terminated.        */
};

/**
 * @brief Parse an AX.25 UI frame into caller-supplied packet storage.
 *
 * Rejects frames that are too short to hold two addresses, that never
 * terminate their address field, that carry more than APRS_MAX_ADDRESSES
 * addresses, or that leave no room for an info field.
 *
 * @param data: raw AX.25 frame, without its frame check sequence.
 * @param len: frame length in bytes, at most APRS_PACLEN.
 * @param pkt: destination packet; untouched if parsing fails.
 * @return true if the frame was parsed, false if it is malformed.
 */
bool aprsPktFromFrame(const uint8_t *data, size_t len, struct aprsPacket *pkt);

/**
 * @brief Parse a single address out of a frame's address field.
 *
 * Lets a caller that only needs one address — the RTX half wants the sender
 * and nothing else — avoid parsing the whole packet. Index 0 is the
 * destination, 1 the source, and 2 onwards the digipeater path.
 *
 * @param data: raw AX.25 frame, without its frame check sequence.
 * @param len: frame length in bytes.
 * @param index: address position, counting from the destination.
 * @param addr: destination address; untouched if the index is out of range.
 * @return true if that address is present in the frame.
 */
bool aprsAddrFromFrame(const uint8_t *data, size_t len, uint8_t index,
                       struct aprsAddress *addr);

/**
 * @brief Format an address as "CALLSIGN" or "CALLSIGN-SSID".
 *
 * The SSID is omitted when it is zero, matching the TNC2 convention.
 *
 * @param addr: address to format.
 * @param out: destination buffer.
 * @param len: size of out; APRS_ADDR_STR_LEN is always enough.
 * @return number of characters written, not counting the NUL.
 */
size_t aprsAddrToStr(const struct aprsAddress *addr, char *out, size_t len);

/**
 * @brief Format the digipeater path as a comma-separated list.
 *
 * Covers addresses 2..addressesLen-1 — everything after destination and
 * source — appending '*' to each address whose has-been-repeated bit is set,
 * exactly as TNC2 monitor output does. Writes an empty string when the packet
 * was heard directly.
 *
 * @param pkt: packet whose path to format.
 * @param out: destination buffer.
 * @param len: size of out; the result is truncated to fit.
 * @return number of characters written, not counting the NUL.
 */
size_t aprsPathToStr(const struct aprsPacket *pkt, char *out, size_t len);

/**
 * @brief Split an addressed message's info field into addressee and text.
 *
 * An APRS message has the form ":ADDRESSEE:text{seq", where ADDRESSEE is
 * padded to exactly nine characters. On success the trailing space padding is
 * removed from the addressee and the message text is returned without the
 * leading ':' and without the optional "{seq" acknowledgement suffix.
 *
 * @param pkt: packet to unwrap; must be APRS_TYPE_MESSAGE.
 * @param addressee: destination for the addressee, NUL-terminated.
 * @param addresseeLen: size of addressee; 10 bytes is always enough.
 * @param text: destination for the message text, NUL-terminated.
 * @param textLen: size of text; the result is truncated to fit.
 * @return true if the info field is a well-formed addressed message.
 */
bool aprsMsgUnwrap(const struct aprsPacket *pkt, char *addressee,
                   size_t addresseeLen, char *text, size_t textLen);

#ifdef __cplusplus
}
#endif

#endif /* APRS_PACKET_H */
