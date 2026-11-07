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
 * @brief Longest message number, up to five alphanumerics per APRS 1.2.
 */
#define APRS_MSG_ID_MAX 5

/**
 * @brief Longest info field of an addressed message or acknowledgement:
 * ":ADDRESSEE:" + text + "{MM}AA".
 */
#define APRS_MSG_INFO_MAX                                   \
    (1 + APRS_MSG_ADDRESSEE_LEN + 1 + APRS_MSG_TEXT_MAX + 2 \
     + (2 * APRS_MSG_ID_MAX))

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
    APRS_TYPE_OTHER = 0,   ///< Unrecognised or unclassified DTI.
    APRS_TYPE_MESSAGE,     ///< ':' addressed text message.
    APRS_TYPE_POSITION,    ///< '!' '=' '/' '@' position report.
    APRS_TYPE_STATUS,      ///< '>' status text.
    APRS_TYPE_OBJECT,      ///< ';' object, ')' item.
    APRS_TYPE_TELEMETRY,   ///< 'T' telemetry report.
    APRS_TYPE_WEATHER,     ///< '_' positionless weather report.
    APRS_TYPE_THIRD_PARTY, ///< '}' a packet relayed for another.
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
 * @brief What an addressed message is.
 */
enum aprsMsgKind {
    APRS_MSG_TEXT = 0, /**< Text, possibly numbered for acknowledgement. */
    APRS_MSG_ACK,      /**< "ackMM": acknowledges message MM.             */
    APRS_MSG_REJ,      /**< "rejMM": rejects message MM.                  */
};

/**
 * @brief Message number and reply-ack of an addressed message.
 *
 * Text ends in "{MM" when it wants an acknowledgement. The reply-ack form
 * "{MM}AA" also acknowledges AA, the sender's last message from us, and a
 * bare "{MM}" says the sender understands reply-acks. An acknowledgement
 * echoes the number it answers, "ackMM" or "ackMM}AA".
 */
struct aprsMsgId {
    enum aprsMsgKind kind;
    char number[APRS_MSG_ID_MAX + 1];   /**< MM, empty if unnumbered. */
    char replyAck[APRS_MSG_ID_MAX + 1]; /**< AA, empty if none.       */
    bool replyAckCapable;               /**< A '}' followed MM.       */
};

/**
 * @brief Read the kind, message number and reply-ack of an addressed message.
 *
 * A malformed number reads as an unnumbered message, which is never
 * acknowledged.
 *
 * @param pkt: packet to read; must be APRS_TYPE_MESSAGE.
 * @param id: destination; untouched if the packet is not a message.
 * @return true if the info field is a well-formed addressed message.
 */
bool aprsMsgParseId(const struct aprsPacket *pkt, struct aprsMsgId *id);

/**
 * @brief Check a message addressee or an APRS-IS source: 1 to 9 upper-case
 * letters, digits and dashes. Wider than aprsCallsignValid(): APRS-IS
 * carries names AX.25 cannot, such as "WINLINK" or "KD7ZYN-S".
 */
bool aprsAddresseeValid(const char *str);

/**
 * @brief Unwrap a third-party packet, "}SRC>DEST,PATH:info" in the info
 * field of the packet that carried it.
 *
 * The inner source comes back as text, since it may be a name AX.25 cannot
 * hold; the inner packet holds the destination and, where they fit AX.25,
 * the source and path elements, with the inner info field and its type.
 *
 * @param pkt: the packet heard.
 * @param inner: destination for the packet it carries.
 * @param src: destination for the inner source.
 * @param srcLen: size of src; APRS_ADDR_STR_LEN is enough.
 * @return true if @p pkt is a well-formed third-party packet.
 */
bool aprsThirdPartyUnwrap(const struct aprsPacket *pkt,
                          struct aprsPacket *inner, char *src, size_t srcLen);

/**
 * @brief Check a digipeater path the user typed: up to eight addresses each
 * valid by aprsCallsignValid(), separated by commas. Empty is valid and
 * means no digipeaters.
 */
bool aprsPathValid(const char *path);

/**
 * @brief What a packet means to this station, beyond its data type.
 *
 * Decided from the whole packet: who it is addressed to, whether it is an
 * acknowledgement or a query, whether it raises an emergency.
 */
enum aprsKind {
    APRS_KIND_OTHER = 0,   /**< Status, telemetry, anything unclassified. */
    APRS_KIND_POSITION,    /**< Position report, object or item.          */
    APRS_KIND_MESSAGE,     /**< Text addressed to some other station.     */
    APRS_KIND_TO_ME,       /**< Text addressed to this station.           */
    APRS_KIND_GROUP,       /**< Bulletin, announcement or group address.  */
    APRS_KIND_ACK,         /**< "ackMM" or "rejMM", whoever it is for.    */
    APRS_KIND_QUERY,       /**< General or directed query.                */
    APRS_KIND_THIRD_PARTY, /**< '}' traffic relayed from another network. */
    APRS_KIND_EMERGENCY,   /**< Mic-E Emergency, or EMERGENCY in the text. */
};

/**
 * @brief Whether a message addressee names a group rather than a station:
 * bulletins and announcements ("BLNn", "BLNx", "BLNnGROUP"), weather
 * bulletins ("NWS-", "SKY"), and the conventional ALL, CQ and QST.
 */
bool aprsGroupAddressee(const char *addressee);

/**
 * @brief Whether a packet raises an emergency: a Mic-E report with the
 * Emergency comment, or the word EMERGENCY anywhere in the text.
 */
bool aprsEmergency(const struct aprsPacket *pkt);

/**
 * @brief Classify a packet for this station.
 *
 * @param pkt: parsed packet.
 * @param me: this station's address, "CALL" or "CALL-SSID"; matched with
 *            the SSID, so that CALL and CALL-7 are different stations.
 * @return the packet's kind.
 */
enum aprsKind aprsPacketKind(const struct aprsPacket *pkt, const char *me);

/**
 * @brief Make text safe to show: control characters become '.'.
 *
 * Bytes from 0x80 up are left alone, so UTF-8 text survives.
 *
 * @param s: NUL-terminated text, changed in place.
 */
void aprsSanitize(char *s);

/** Which fields of a weather report were sent. */
enum aprsWeatherValid {
    APRS_WX_WIND_DIR = 1,
    APRS_WX_WIND_SPEED = 2,
    APRS_WX_GUST = 4,
    APRS_WX_TEMP = 8,
    APRS_WX_RAIN_1H = 16,
    APRS_WX_RAIN_24H = 32,
    APRS_WX_RAIN_TODAY = 64,
    APRS_WX_HUMIDITY = 128,
    APRS_WX_PRESSURE = 256,
};

/** A weather report, in the units APRS sends. */
struct aprsWeather {
    uint16_t valid;     /**< enum aprsWeatherValid bits.                */
    uint16_t windDir;   /**< Degrees.                                   */
    uint16_t windSpeed; /**< mph, one-minute sustained.                 */
    uint16_t gust;      /**< mph, peak of the last five minutes.        */
    int16_t temp;       /**< Fahrenheit.                                */
    uint16_t rain1h;    /**< Hundredths of an inch.                     */
    uint16_t rain24h;   /**< Hundredths of an inch.                     */
    uint16_t rainToday; /**< Hundredths of an inch since midnight.      */
    uint8_t humidity;   /**< Percent.                                   */
    uint16_t pressure;  /**< Tenths of a millibar.                      */
};

/**
 * @brief Read a weather report: positionless ("_MMDDHHMMc...s...") or
 * after a position with the weather station symbol ("_"), where the
 * course/speed slot holds the wind.
 *
 * @param pkt: parsed packet.
 * @param wx: destination; untouched if the packet carries no weather.
 * @return true if at least one field was read.
 */
bool aprsWeatherParse(const struct aprsPacket *pkt, struct aprsWeather *wx);

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
 * @brief Format an addressed text message with a message number.
 *
 * As aprsMsgFormat(), followed by "{MM" and, when @p id is reply-ack capable,
 * "}" and its reply-ack.
 *
 * @param id: number to append; NULL or an empty number appends nothing.
 * @return number of characters written, or 0 as for aprsMsgFormat() or if
 *         the number is malformed.
 */
size_t aprsMsgFormatId(char *out, size_t len, const char *addressee,
                       const char *text, const struct aprsMsgId *id);

/**
 * @brief Format an acknowledgement or rejection of a received message.
 *
 * Echoes the number exactly as received, "MM" or "MM}AA", as APRS 1.2
 * requires.
 *
 * @param out: destination buffer; APRS_MSG_INFO_MAX + 1 is always enough.
 * @param len: size of out.
 * @param addressee: station that sent the message.
 * @param kind: APRS_MSG_ACK or APRS_MSG_REJ.
 * @param id: number of the message being answered.
 * @return number of characters written, or 0 if the arguments are invalid.
 */
size_t aprsAckFormat(char *out, size_t len, const char *addressee,
                     enum aprsMsgKind kind, const struct aprsMsgId *id);

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

/**
 * @brief A position read out of a received packet, in gps_t units.
 *
 * The inverse of aprsPosReport: micro-degrees, km/h, metres, with each
 * optional field flagged rather than given a sentinel, since a course of
 * zero is due north.
 */
struct aprsPosition {
    char name[10];     /**< Object or item name; empty for a station.   */
    bool killed;       /**< An object or item being taken off the map.  */
    bool emergency;    /**< Mic-E with the Emergency position comment.  */
    int32_t latitude;  /**< Millionths of a degree, positive north.     */
    int32_t longitude; /**< Millionths of a degree, positive east.      */
    bool ambiguous;    /**< The sender withheld some digits.            */
    char symbolTable;
    char symbolCode;
    bool hasCourseSpeed;
    uint16_t course;     /**< Degrees, 0-359.    */
    uint16_t speed;      /**< km/h.              */
    bool hasAltitude;
    int16_t altitude;    /**< Metres.            */
    bool hasPhg;         /**< "PHGphgd": power, height, gain, direction. */
    uint16_t phgPower;   /**< Watts.                                     */
    uint16_t phgHeight;  /**< Feet above average terrain.                */
    uint8_t phgGain;     /**< dB.                                        */
    uint16_t phgDir;     /**< Degrees the antenna favours; 0 omni.       */
    uint16_t range;      /**< Miles, from "RNGrrrr"; 0 if not sent.      */
    bool hasDao;         /**< "!DAO!" added a digit of precision.        */
    const char *comment; /**< Into pkt->info, not NUL-terminated.     */
    uint16_t commentLen; /**< Comment length.                         */
};

/**
 * @brief Read the position a packet carries.
 *
 * Handles position reports with and without a timestamp, uncompressed and
 * compressed, Mic-E, objects and items, the course/speed extension and the
 * "/A=" altitude.
 *
 * @param pkt: parsed packet.
 * @param pos: destination; untouched if the packet carries no position.
 * @return true if a position was read.
 */
bool aprsPosParse(const struct aprsPacket *pkt, struct aprsPosition *pos);

/** Access tone a frequency announcement asks for. */
enum aprsTone {
    APRS_TONE_NONE = 0,
    APRS_TONE_ENCODE,  /**< "Txxx": a CTCSS tone sent on transmit. */
    APRS_TONE_SQUELCH, /**< "Cxxx": the tone both ways.            */
    APRS_TONE_DCS,     /**< "Dxxx".                                */
    APRS_TONE_BURST,   /**< "1750": a 1750 Hz tone burst.          */
};

/** Mode a frequency is worked in, from words in the comment. */
enum aprsFreqMode {
    APRS_FREQ_FM = 0,
    APRS_FREQ_M17,
    APRS_FREQ_DMR,
    APRS_FREQ_DSTAR,
    APRS_FREQ_FUSION,
    APRS_FREQ_P25,
    APRS_FREQ_NXDN,
    APRS_FREQ_DATA, /**< Packet, VARA and other Winlink gateways. */
};

enum aprsFreqFlags {
    APRS_FREQ_HAS_OFFSET = 1, /**< Sent; else the band's standard applies. */
    APRS_FREQ_NARROW = 2,     /**< A lower-case tone: 12.5 kHz FM.        */
    APRS_FREQ_HAS_CAN = 4,
};

/** A frequency a station announces, per aprs.org/info/freqspec.txt. */
struct aprsFreq {
    uint32_t freq;        /**< Hz, where the station transmits.       */
    int32_t offset;       /**< Hz added to transmit to it, if sent.   */
    uint16_t tone;        /**< Whole Hz, "107" is 107.2; or DCS code. */
    uint8_t toneType : 4; /**< enum aprsTone.                         */
    uint8_t mode     : 4; /**< enum aprsFreqMode.                     */
    uint8_t can      : 4; /**< M17 channel access number.             */
    uint8_t flags    : 4; /**< enum aprsFreqFlags.                    */
};

/**
 * @brief Read the frequency a packet announces.
 *
 * Kenwood's TUNE reads the same fields: an object named for the frequency
 * ("146.82TN"), or "FFF.FFFMHz" opening a position, object or status
 * comment; then a tone ("T107", "C107", "D023", "1750", lower case for
 * narrow), an offset in 10 kHz ("-060", "Toff" for simplex) and words
 * naming the mode ("M17 CAN0", "DMR", "Fusion").
 *
 * @param pkt: parsed packet.
 * @param f: destination; untouched if no frequency is announced.
 * @return true if a frequency was read.
 */
bool aprsFreqParse(const struct aprsPacket *pkt, struct aprsFreq *f);

#ifdef __cplusplus
}
#endif

#endif /* APRS_PACKET_H */
