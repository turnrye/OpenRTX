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
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Longest formatted "CALLSIGN-SSID" string, with its NUL: six callsign
 * characters, a dash and two SSID digits.
 */
#define APRS_ADDR_STR_LEN 10

/**
 * AX.25 destination of every frame OpenRTX transmits, the TOCALL receiving
 * stations read to tell which software sent it. `APORT?` is registered to
 * OpenRTX in aprsorg/aprs-deviceid, with the last character ours to assign;
 * the digit marks the firmware generation.
 */
#define APRS_TOCALL "APORT1"

/**
 * Digipeater path for frames OpenRTX transmits: one local hop, one wide hop.
 */
#define APRS_DEFAULT_PATH "WIDE1-1,WIDE2-1"

/**
 * Longest text an addressed ":ADDRESSEE:text" message carries.
 */
#define APRS_MSG_TEXT_MAX 67

/**
 * Length of a message's addressee field, always space-padded to it.
 */
#define APRS_MSG_ADDRESSEE_LEN 9

/**
 * One AX.25 address: callsign, SSID, and the has-been-repeated bit.
 */
// clang-format off
struct aprsAddress {
    char addr[7];           ///< Callsign, unshifted and NUL-terminated.
    uint8_t ssid      : 4,  ///< Secondary station identifier, 0-15.
            repeated  : 1,  ///< H bit: this digipeater has repeated it.
            _reserved : 3;
};
// clang-format on

/**
 * APRS data type, from the data type identifier (DTI) that opens the info
 * field. Anything not named is APRS_TYPE_OTHER and keeps its raw text.
 */
enum aprsType {
    APRS_TYPE_OTHER = 0, ///< Unrecognised or unclassified DTI.
    APRS_TYPE_MESSAGE,   ///< ':' addressed text message.
    APRS_TYPE_POSITION,  ///< '!' '=' '/' '@' position report.
    APRS_TYPE_STATUS,    ///< '>' status text.
    APRS_TYPE_OBJECT,    ///< ';' object, ')' item.
    APRS_TYPE_TELEMETRY, ///< 'T' telemetry report.
};

/**
 * A parsed APRS packet. Its info field points into the frame it was parsed
 * from, so it is valid only as long as that frame is.
 */
struct aprsPacket {
    const char *info;     ///< Info field, inside the frame; not NUL-terminated.
    struct aprsAddress addresses[APRS_MAX_ADDRESSES];
    enum aprsType type;   ///< Classification of info[0].
    uint16_t infoLen;     ///< Info field length.
    uint8_t addressesLen; ///< Addresses actually present.
};

/**
 * Parse an AX.25 UI frame into caller-supplied packet storage.
 *
 * Rejects frames too short for two addresses, whose address field never ends
 * or holds more than APRS_MAX_ADDRESSES addresses, or that have no info field
 * or one longer than APRS_MAX_INFO.
 *
 * @param data: raw AX.25 frame, without its FCS.
 * @param len: frame length in bytes.
 * @param pkt: destination packet, pointing into data; untouched if
 * parsing fails.
 * @return 0 on success, -EINVAL if the frame is malformed.
 */
int aprsPkt_fromFrame(const uint8_t *data, size_t len, struct aprsPacket *pkt);

/**
 * Format an address as "CALLSIGN", or "CALLSIGN-SSID" when the SSID is not
 * zero.
 *
 * @param addr: address to format.
 * @param out: destination buffer.
 * @param len: size of out; APRS_ADDR_STR_LEN is always enough.
 * @return number of characters written, not counting the NUL.
 */
size_t aprsAddr_toStr(const struct aprsAddress *addr, char *out, size_t len);

/**
 * Format the digipeater path as a comma-separated list, with '*' after each
 * address that has repeated the frame, as TNC2 monitors do. Empty when the
 * packet was heard directly.
 *
 * @param pkt: packet whose path to format.
 * @param out: destination buffer.
 * @param len: size of out; the result is truncated to fit.
 * @return number of characters written, not counting the NUL.
 */
size_t aprsPath_toStr(const struct aprsPacket *pkt, char *out, size_t len);

/**
 * Split an addressed message ":ADDRESSEE:text{seq" into its addressee,
 * without the padding, and its text, without the "{seq" suffix.
 *
 * @param pkt: packet to unwrap.
 * @param addressee: destination for the addressee, NUL-terminated.
 * @param addresseeLen: size of addressee; APRS_ADDR_STR_LEN is enough.
 * @param text: destination for the text, NUL-terminated and truncated to fit.
 * @param textLen: size of text.
 * @return 0 on success, -EINVAL if the packet is not an addressed message.
 */
int aprsMsg_unwrap(const struct aprsPacket *pkt, char *addressee,
                   size_t addresseeLen, char *text, size_t textLen);

/**
 * Parse a "CALLSIGN" or "CALLSIGN-SSID" string into an address, upper-casing
 * the callsign. Lenient, as text received from other stations can be:
 * "k1abc-07" is K1ABC-7.
 *
 * @param str: address text, NUL-terminated.
 * @param addr: destination address; untouched if the text is not an address.
 * @return 0 on success, -EINVAL if the text is not an AX.25 address.
 */
int aprsAddr_fromStr(const char *str, struct aprsAddress *addr);

/**
 * Check a callsign the user chooses: one to six upper-case letters or
 * digits, optionally followed by "-" and an SSID of 0 to 15 without a
 * leading zero, as APRS writes station addresses. An M17 callsign is not
 * necessarily one: M17 allows nine characters and '/' and '.'.
 *
 * @param str: callsign text, NUL-terminated.
 * @return 0 if the text is a valid station address, -EINVAL otherwise.
 */
int aprsAddr_validate(const char *str);

/**
 * Build an AX.25 UI frame from TNC2-style address text: addresses, control,
 * PID and info, without the FCS, as the decoder hands a frame back.
 *
 * @param buf: destination buffer; untouched if the frame cannot be built.
 * @param cap: size of buf.
 * @param dest: destination address, the TOCALL for a transmitted frame.
 * @param src: source address, "CALLSIGN" or "CALLSIGN-SSID".
 * @param path: comma-separated digipeater path; NULL or "" for none.
 * @param info: info field bytes; need not be NUL-terminated.
 * @param infoLen: number of info bytes, 1 to APRS_MAX_INFO.
 * @return frame length on success, -EINVAL for an address or path that is not
 *         valid AX.25 or an info length out of range, -ENOSPC if the frame
 *         does not fit in buf.
 */
ssize_t aprsFrame_build(uint8_t *buf, size_t cap, const char *dest,
                        const char *src, const char *path, const char *info,
                        size_t infoLen);

/**
 * Format the info field of an addressed message, ":ADDRESSEE:text", with the
 * addressee space-padded to APRS_MSG_ADDRESSEE_LEN.
 *
 * @param out: destination buffer, NUL-terminated on success.
 * @param len: size of out.
 * @param addressee: recipient callsign, at most APRS_MSG_ADDRESSEE_LEN
 *                   characters.
 * @param text: message text, at most APRS_MSG_TEXT_MAX characters.
 * @return number of characters written on success, -EINVAL if the addressee
 *         or text is too long or the addressee empty, -ENOSPC if out is too
 *         small.
 */
ssize_t aprsMsg_format(char *out, size_t len, const char *addressee,
                       const char *text);

/**
 * @brief Check a digipeater path the user typed: up to eight addresses each
 * valid by aprsCallsignValid(), separated by commas. Empty is valid and
 * means no digipeaters.
 */
bool aprsPathValid(const char *path);

/**
 * @brief Write an AX.25 UI frame as a TNC2 monitor line,
 * "SRC>DEST,PATH:info", with only the last address a digipeater has used
 * marked '*', as the format has it.
 *
 * @param frame: AX.25 frame without its FCS.
 * @param len: its length.
 * @param out: destination, NUL-terminated on success.
 * @param cap: size of out.
 * @return line length without the NUL, or 0 if the frame is malformed or
 *         the line does not fit.
 */
size_t aprsFrameToTnc2(const uint8_t *frame, size_t len, char *out, size_t cap);

/**
 * @brief Build an AX.25 UI frame from a TNC2 monitor line,
 * "SRC>DEST,PATH:info", the inverse of how traffic is shown.
 *
 * @param line: the line, not necessarily NUL-terminated.
 * @param len: its length.
 * @param buf: destination for the frame, without its FCS.
 * @param cap: size of buf.
 * @return frame length, or 0 if the line is malformed or does not fit.
 */
size_t aprsFrameFromTnc2(const char *line, size_t len, uint8_t *buf,
                         size_t cap);

/**
 * @brief An uncompressed APRS position report, before it is formatted.
 *
 * Positions are held the way the GPS layer reports them — latitude and
 * longitude in millionths of a degree, altitude in metres, speed in km/h —
 * so a caller can hand a gps_t snapshot straight in. aprsPosFormat() does the
 * conversion to the units APRS puts on the air (minutes, feet, knots).
 */
struct aprsPosReport {
    int32_t latitude;    /**< Millionths of a degree, positive north. */
    int32_t longitude;   /**< Millionths of a degree, positive east.  */
    char symbolTable;    /**< Symbol table id, e.g. '/' (primary).    */
    char symbolCode;     /**< Symbol code, e.g. '>' (car).            */
    bool hasCourseSpeed; /**< Emit the course/speed extension.        */
    uint16_t course;     /**< Course over ground, degrees, 0-360.     */
    uint16_t speed;      /**< Ground speed in km/h; emitted as knots. */
    bool hasAltitude;    /**< Emit the /A= altitude extension.        */
    int16_t altitude;    /**< Altitude in metres MSL; emitted as feet.*/
    const char *comment; /**< Trailing free text, or NULL for none.   */
};

/**
 * @brief Format the info field of an uncompressed position report.
 *
 * Produces "=DDMM.hhN/DDDMM.hhW$..." — the with-messaging, no-timestamp
 * position report ('=') that a beacon transmits. The coordinate is written to
 * hundredth-of-a-minute resolution (about 18.5 m) with correct rounding, the
 * optional course/speed and altitude extensions follow in that order, and any
 * comment comes last.
 *
 * Optional fields are dropped, whole, to keep the result within @p len: the
 * comment goes first, then altitude, then course/speed. The coordinate and
 * symbol always fit — a caller must give at least 21 bytes — and a comment
 * that does not fit is truncated rather than dropping the fields before it.
 *
 * @param out: destination buffer for the info field, NUL-terminated.
 * @param len: size of out; at least 21 bytes for the coordinate alone.
 * @param rpt: the position report to format.
 * @return number of characters written, not counting the NUL, or 0 if the
 *         arguments are invalid or the coordinate does not fit.
 */
size_t aprsPosFormat(char *out, size_t len, const struct aprsPosReport *rpt);

#ifdef __cplusplus
}
#endif

#endif /* APRS_PACKET_H */
