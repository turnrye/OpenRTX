/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/APRS/packet.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/*
 * Bits of the last byte of an AX.25 address, laid out as CRRSSSSL: the
 * command bit on the destination and source, or the has-been-repeated bit on
 * a digipeater; two reserved bits; the SSID; and the last-address marker.
 */
enum addrFlags {
    ADDR_LAST = 0x01,      ///< Last address of the address field.
    ADDR_SSID_SHIFT = 1,   ///< Position of the SSID.
    ADDR_SSID_MASK = 0x1e, ///< Four-bit SSID.
    ADDR_RESERVED = 0x60,  ///< Reserved bits, sent as ones.
    ADDR_CMD_OR_H = 0x80,  ///< Command bit, or H bit on a digipeater.
};

/*
 * Control and protocol identifier of an APRS frame: a UI frame carrying no
 * layer 3 protocol.
 */
enum ax25Ui {
    AX25_CTRL_UI = 0x03,   ///< Unnumbered information frame.
    AX25_PID_NO_L3 = 0xf0, ///< No layer 3 protocol.
};

/* ":ADDRESSEE:" — the separator after the padded addressee is at a fixed
 * offset. */
#define MSG_TEXT_OFFSET (1 + APRS_MSG_ADDRESSEE_LEN + 1)

/*
 * Classify the data type identifier that opens an APRS info field.
 */
static enum aprsType classifyDti(char dti)
{
    switch (dti) {
        case ':':
            return APRS_TYPE_MESSAGE;
        case '!':
        case '=':
        case '/':
        case '@':
        case '`': /* Mic-E, current and old data */
        case '\'':
        case 0x1c:
        case 0x1d:
            return APRS_TYPE_POSITION;
        case '>':
            return APRS_TYPE_STATUS;
        case ';':
        case ')':
            return APRS_TYPE_OBJECT;
        case 'T':
            return APRS_TYPE_TELEMETRY;
        case '_':
            return APRS_TYPE_WEATHER;
        case '}':
            return APRS_TYPE_THIRD_PARTY;
        default:
            return APRS_TYPE_OTHER;
    }
}

/*
 * Unshift one seven-byte AX.25 address. The padding spaces, and any other
 * byte that does not unshift to a printable character, are dropped.
 */
static void parseAddress(const uint8_t *src, struct aprsAddress *addr)
{
    const uint8_t flags = src[APRS_ADDR_LEN - 1];
    size_t j = 0;

    for (size_t k = 0; k < APRS_ADDR_LEN - 1; k++) {
        uint8_t c = src[k] >> 1;
        if ((c > ' ') && (c < 0x7f))
            addr->addr[j++] = (char)c;
    }

    addr->addr[j] = '\0';
    addr->ssid = (flags & ADDR_SSID_MASK) >> ADDR_SSID_SHIFT;
    addr->repeated = ((flags & ADDR_CMD_OR_H) != 0) ? 1 : 0;
}

/*
 * Count the addresses a frame carries, or 0 if its address field is
 * malformed: shorter than a destination and a source, never terminated, or
 * longer than AX.25 allows.
 */
static size_t countAddresses(const uint8_t *data, size_t len)
{
    size_t offset = 0;
    size_t count = 0;
    bool last = false;

    while (((len - offset) >= APRS_ADDR_LEN) && !last) {
        if (count == APRS_MAX_ADDRESSES)
            return 0;

        last = (data[offset + APRS_ADDR_LEN - 1] & ADDR_LAST) != 0;
        offset += APRS_ADDR_LEN;
        count++;
    }

    if (!last || (count < 2))
        return 0;

    return count;
}

int aprsPkt_fromFrame(const uint8_t *data, size_t len, struct aprsPacket *pkt)
{
    /* Check the whole frame before writing anything, so a malformed one
     * leaves the caller's packet untouched. */
    const size_t count = countAddresses(data, len);
    if (count == 0)
        return -EINVAL;

    const size_t infoOffset = (count * APRS_ADDR_LEN) + APRS_CTRL_PID_LEN;
    if (infoOffset >= len)
        return -EINVAL;

    const size_t infoLen = len - infoOffset;
    if (infoLen > APRS_MAX_INFO)
        return -EINVAL;

    memset(pkt, 0, sizeof(*pkt));
    pkt->addressesLen = (uint8_t)count;
    pkt->infoLen = (uint16_t)infoLen;

    for (size_t i = 0; i < count; i++)
        parseAddress(&data[i * APRS_ADDR_LEN], &pkt->addresses[i]);

    pkt->info = (const char *)&data[infoOffset];
    pkt->type = classifyDti(pkt->info[0]);

    return 0;
}

size_t aprsAddr_toStr(const struct aprsAddress *addr, char *out, size_t len)
{
    int written;

    if (addr->ssid != 0)
        written = snprintf(out, len, "%s-%u", addr->addr, addr->ssid);
    else
        written = snprintf(out, len, "%s", addr->addr);

    if (written < 0) {
        out[0] = '\0';
        return 0;
    }

    return ((size_t)written < len) ? (size_t)written : (len - 1);
}

size_t aprsPath_toStr(const struct aprsPacket *pkt, char *out, size_t len)
{
    size_t used = 0;

    out[0] = '\0';

    for (size_t i = 2; i < pkt->addressesLen; i++) {
        if ((used + 1) >= len)
            break;

        if (used > 0)
            out[used++] = ',';

        used += aprsAddr_toStr(&pkt->addresses[i], out + used, len - used);

        if ((pkt->addresses[i].repeated != 0) && ((used + 1) < len))
            out[used++] = '*';

        out[used] = '\0';
    }

    return used;
}

int aprsMsg_unwrap(const struct aprsPacket *pkt, char *addressee,
                   size_t addresseeLen, char *text, size_t textLen)
{
    /* The separator after the fixed-width addressee tells a message from any
     * other info field that merely starts with ':'. */
    if ((pkt->type != APRS_TYPE_MESSAGE) || (pkt->infoLen < MSG_TEXT_OFFSET)
        || (pkt->info[MSG_TEXT_OFFSET - 1] != ':'))
        return -EINVAL;

    size_t n = APRS_MSG_ADDRESSEE_LEN;
    while ((n > 0) && (pkt->info[n] == ' '))
        n--;

    if (n == 0)
        return -EINVAL;
    if (n >= addresseeLen)
        n = addresseeLen - 1;

    memcpy(addressee, &pkt->info[1], n);
    addressee[n] = '\0';

    /* The "{seq" suffix of an acknowledged message is not part of the
     * text. */
    const char *body = &pkt->info[MSG_TEXT_OFFSET];
    size_t bodyLen = pkt->infoLen - MSG_TEXT_OFFSET;
    const char *brace = memchr(body, '{', bodyLen);
    if (brace != NULL)
        bodyLen = (size_t)(brace - body);

    if (bodyLen >= textLen)
        bodyLen = textLen - 1;

    memcpy(text, body, bodyLen);
    text[bodyLen] = '\0';

    return 0;
}

/*
 * Upper-case an ASCII letter, leaving anything else as it is.
 */
static char upcase(char c)
{
    if ((c >= 'a') && (c <= 'z'))
        return (char)(c - 'a' + 'A');

    return c;
}

int aprsAddr_fromStr(const char *str, struct aprsAddress *addr)
{
    struct aprsAddress parsed;
    size_t i = 0;

    memset(&parsed, 0, sizeof(parsed));

    for (; (str[i] != '\0') && (str[i] != '-'); i++) {
        if (i >= (APRS_ADDR_LEN - 1))
            return -EINVAL;

        /* Only printable characters survive the shift, and a space would
         * end the callsign early. */
        char c = upcase(str[i]);
        if ((c <= ' ') || (c >= 0x7f))
            return -EINVAL;

        parsed.addr[i] = c;
    }

    if (i == 0)
        return -EINVAL;

    parsed.addr[i] = '\0';

    if (str[i] == '-') {
        const char *digits = &str[i + 1];
        uint32_t ssid = 0;

        if (digits[0] == '\0')
            return -EINVAL;

        for (size_t j = 0; digits[j] != '\0'; j++) {
            if ((digits[j] < '0') || (digits[j] > '9'))
                return -EINVAL;

            ssid = (ssid * 10) + (uint32_t)(digits[j] - '0');
            if (ssid > 15)
                return -EINVAL;
        }

        parsed.ssid = ssid;
    }

    *addr = parsed;
    return 0;
}

int aprsAddr_validate(const char *str)
{
    size_t i = 0;

    for (; (str[i] != '\0') && (str[i] != '-'); i++) {
        const char c = str[i];
        const bool alnum = ((c >= 'A') && (c <= 'Z'))
                        || ((c >= '0') && (c <= '9'));

        if ((i >= (APRS_ADDR_LEN - 1)) || !alnum)
            return -EINVAL;
    }

    if (i == 0)
        return -EINVAL;
    if (str[i] == '\0')
        return 0;

    /* "-N", or "-1N" up to 15: no leading zero */
    const char *ssid = &str[i + 1];
    const size_t len = strlen(ssid);
    const bool digit = (ssid[0] >= '0') && (ssid[0] <= '9');

    if ((len == 1) && digit)
        return 0;
    if ((len == 2) && (ssid[0] == '1') && (ssid[1] >= '0') && (ssid[1] <= '5'))
        return 0;

    return -EINVAL;
}

/*
 * Shift one address into the seven bytes AX.25 sends. Short callsigns are
 * padded with spaces, and the reserved bits are sent as ones, as every other
 * station does.
 */
static void writeAddress(uint8_t *dst, const struct aprsAddress *addr,
                         bool command, bool last)
{
    size_t i = 0;

    for (; (i < APRS_ADDR_LEN - 1) && (addr->addr[i] != '\0'); i++)
        dst[i] = (uint8_t)(addr->addr[i] << 1);
    for (; i < APRS_ADDR_LEN - 1; i++)
        dst[i] = (uint8_t)(' ' << 1);

    uint8_t flags = ADDR_RESERVED | (uint8_t)(addr->ssid << ADDR_SSID_SHIFT);
    if ((addr->repeated != 0) || command)
        flags |= ADDR_CMD_OR_H;
    if (last)
        flags |= ADDR_LAST;

    dst[APRS_ADDR_LEN - 1] = flags;
}

/*
 * Parse a comma-separated digipeater path into @p addresses, each element
 * optionally followed by the '*' that aprsPath_toStr() writes for a repeated
 * one. Returns the number of addresses parsed, or -EINVAL.
 */
static int parsePath(const char *path, struct aprsAddress *addresses,
                     size_t room)
{
    const char *p = path;
    size_t count = 0;

    while (*p != '\0') {
        const char *comma = strchr(p, ',');
        size_t len = (comma != NULL) ? (size_t)(comma - p) : strlen(p);
        char element[APRS_ADDR_STR_LEN];
        bool repeated = false;

        if (count == room)
            return -EINVAL;

        if ((len > 0) && (p[len - 1] == '*')) {
            repeated = true;
            len--;
        }

        if ((len == 0) || (len >= sizeof(element)))
            return -EINVAL;

        memcpy(element, p, len);
        element[len] = '\0';

        if (aprsAddr_fromStr(element, &addresses[count]) != 0)
            return -EINVAL;

        addresses[count].repeated = repeated ? 1 : 0;
        count++;

        if (comma == NULL)
            break;

        p = comma + 1;
        if (*p == '\0')
            return -EINVAL;
    }

    return (int)count;
}

ssize_t aprsFrame_build(uint8_t *buf, size_t cap, const char *dest,
                        const char *src, const char *path, const char *info,
                        size_t infoLen)
{
    struct aprsAddress addresses[APRS_MAX_ADDRESSES];

    if ((infoLen == 0) || (infoLen > APRS_MAX_INFO))
        return -EINVAL;

    /* Parse every address before writing anything, so a bad one leaves the
     * caller's buffer untouched. */
    if ((aprsAddr_fromStr(dest, &addresses[0]) != 0)
        || (aprsAddr_fromStr(src, &addresses[1]) != 0))
        return -EINVAL;

    int hops = 0;
    if (path != NULL)
        hops = parsePath(path, &addresses[2], APRS_MAX_ADDRESSES - 2);

    if (hops < 0)
        return hops;

    const size_t count = 2 + (size_t)hops;
    const size_t total = (count * APRS_ADDR_LEN) + APRS_CTRL_PID_LEN + infoLen;
    if (total > cap)
        return -ENOSPC;

    /* APRS sends UI frames as commands: the command bit is set on the
     * destination. */
    for (size_t i = 0; i < count; i++)
        writeAddress(&buf[i * APRS_ADDR_LEN], &addresses[i], (i == 0),
                     (i == (count - 1)));

    size_t offset = count * APRS_ADDR_LEN;
    buf[offset++] = AX25_CTRL_UI;
    buf[offset++] = AX25_PID_NO_L3;
    memcpy(&buf[offset], info, infoLen);

    return (ssize_t)total;
}

ssize_t aprsMsg_format(char *out, size_t len, const char *addressee,
                       const char *text)
{
    const size_t addrLen = strlen(addressee);
    const size_t textLen = strlen(text);

    if ((addrLen == 0) || (addrLen > APRS_MSG_ADDRESSEE_LEN)
        || (textLen > APRS_MSG_TEXT_MAX))
        return -EINVAL;

    const size_t total = MSG_TEXT_OFFSET + textLen;
    if (len < (total + 1))
        return -ENOSPC;

    out[0] = ':';
    memcpy(&out[1], addressee, addrLen);
    memset(&out[1 + addrLen], ' ', APRS_MSG_ADDRESSEE_LEN - addrLen);
    out[MSG_TEXT_OFFSET - 1] = ':';
    memcpy(&out[MSG_TEXT_OFFSET], text, textLen);
    out[total] = '\0';

    return (ssize_t)total;
}

static bool isDigit(char c);

static bool isAlnum(char c)
{
    return ((c >= '0') && (c <= '9')) || ((c >= 'A') && (c <= 'Z'))
        || ((c >= 'a') && (c <= 'z'));
}

/**
 * Read "MM" or "MM}AA" from @p s, which must be exactly that long.
 */
static bool parseNumber(const char *s, size_t len, struct aprsMsgId *id)
{
    size_t mm = 0;
    while ((mm < len) && isAlnum(s[mm]))
        mm++;

    if ((mm == 0) || (mm > APRS_MSG_ID_MAX))
        return false;

    size_t aa = 0;
    bool capable = (mm < len) && (s[mm] == '}');
    if (capable) {
        while ((mm + 1 + aa < len) && isAlnum(s[mm + 1 + aa]))
            aa++;
        if (aa > APRS_MSG_ID_MAX)
            return false;
    }

    if ((mm + (capable ? 1 + aa : 0)) != len)
        return false;

    memcpy(id->number, s, mm);
    id->number[mm] = '\0';
    id->replyAck[0] = '\0';
    if (capable) {
        memcpy(id->replyAck, &s[mm + 1], aa);
        id->replyAck[aa] = '\0';
    }
    id->replyAckCapable = capable;

    return true;
}

bool aprsMsgParseId(const struct aprsPacket *pkt, struct aprsMsgId *id)
{
    if ((pkt == NULL) || (id == NULL) || (pkt->type != APRS_TYPE_MESSAGE))
        return false;
    if ((pkt->infoLen < MSG_TEXT_OFFSET)
        || (pkt->info[MSG_TEXT_OFFSET - 1] != ':'))
        return false;

    const char *body = &pkt->info[MSG_TEXT_OFFSET];
    size_t len = pkt->infoLen - MSG_TEXT_OFFSET;

    /* Some stations end the line with spaces or a CR. */
    while ((len > 0)
           && ((body[len - 1] == ' ') || (body[len - 1] == '\r')
               || (body[len - 1] == '\n')))
        len--;

    struct aprsMsgId parsed;
    memset(&parsed, 0, sizeof(parsed));
    parsed.kind = APRS_MSG_TEXT;

    const char *brace = memchr(body, '{', len);
    if (brace != NULL) {
        size_t off = (size_t)(brace - body) + 1;
        if (!parseNumber(&body[off], len - off, &parsed))
            memset(&parsed, 0, sizeof(parsed));
    } else if ((len > 3)
               && ((memcmp(body, "ack", 3) == 0)
                   || (memcmp(body, "rej", 3) == 0))) {
        if (parseNumber(&body[3], len - 3, &parsed))
            parsed.kind = (body[0] == 'a') ? APRS_MSG_ACK : APRS_MSG_REJ;
        else
            memset(&parsed, 0, sizeof(parsed));
    }

    *id = parsed;
    return true;
}

/* ------------------------------------------------------------------------ *
 * Frame construction — the inverse of everything above.
 * ------------------------------------------------------------------------ */

bool aprsAddresseeValid(const char *str)
{
    if (str == NULL)
        return false;

    size_t n = strlen(str);
    if ((n == 0) || (n >= APRS_ADDR_STR_LEN))
        return false;

    for (size_t i = 0; i < n; i++) {
        char c = str[i];
        if (!(((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9'))
              || (c == '-')))
            return false;
    }

    return true;
}

bool aprsThirdPartyUnwrap(const struct aprsPacket *pkt,
                          struct aprsPacket *inner, char *src, size_t srcLen)
{
    static struct aprsPacket p;
    char header[APRS_MAX_ADDRESSES * APRS_ADDR_STR_LEN];

    if ((pkt == NULL) || (inner == NULL) || (src == NULL) || (srcLen == 0)
        || (pkt->infoLen < 2) || (pkt->info[0] != '}'))
        return false;

    /* No address contains a colon: the first one ends the header. */
    const char *line = &pkt->info[1];
    size_t len = pkt->infoLen - 1;
    const char *colon = memchr(line, ':', len);
    if ((colon == NULL) || ((size_t)(colon - line) >= sizeof(header)))
        return false;

    size_t hlen = (size_t)(colon - line);
    memcpy(header, line, hlen);
    header[hlen] = '\0';

    char *gt = strchr(header, '>');
    if (gt == NULL)
        return false;
    *gt = '\0';

    char *dest = gt + 1;
    char *path = strchr(dest, ',');
    if (path != NULL)
        *path++ = '\0';

    if (!aprsAddresseeValid(header) || (strlen(header) >= srcLen))
        return false;

    memset(&p, 0, sizeof(p));
    if (aprsAddr_fromStr(dest, &p.addresses[0]) != 0)
        return false;
    if (aprsAddr_fromStr(header, &p.addresses[1]) != 0)
        memset(&p.addresses[1], 0, sizeof(p.addresses[1]));
    p.addressesLen = 2;

    /* The path as far as AX.25 can hold it, for the record. */
    while ((path != NULL) && (*path != '\0')
           && (p.addressesLen < APRS_MAX_ADDRESSES)) {
        char *next = strchr(path, ',');
        if (next != NULL)
            *next++ = '\0';

        size_t n = strlen(path);
        bool used = (n > 0) && (path[n - 1] == '*');
        if (used)
            path[n - 1] = '\0';

        struct aprsAddress hop;
        memset(&hop, 0, sizeof(hop));
        if (aprsAddr_fromStr(path, &hop) == 0) {
            hop.repeated = used ? 1 : 0;
            p.addresses[p.addressesLen++] = hop;
        }

        path = next;
    }

    size_t infoLen = len - hlen - 1;
    if ((infoLen == 0) || (infoLen > APRS_MAX_INFO))
        return false;

    // The inner packet points into the outer one's info field
    p.info = colon + 1;
    p.infoLen = (uint16_t)infoLen;
    p.type = classifyDti(p.info[0]);

    *inner = p;
    strcpy(src, header);
    return true;
}

/**
 * Write "MM" or "MM}AA" for @p id into @p out, NUL-terminated.
 *
 * @return characters written, or 0 if the number is malformed or too long.
 */
static size_t formatNumber(char *out, size_t len, const struct aprsMsgId *id)
{
    struct aprsMsgId check;
    char buf[(2 * APRS_MSG_ID_MAX) + 2];
    size_t mm = strnlen(id->number, sizeof(id->number));
    size_t aa = id->replyAckCapable ?
                    strnlen(id->replyAck, sizeof(id->replyAck)) :
                    0;

    if ((mm >= sizeof(id->number)) || (aa >= sizeof(id->replyAck)))
        return 0;

    memcpy(buf, id->number, mm);
    size_t n = mm;
    if (id->replyAckCapable) {
        buf[n++] = '}';
        memcpy(&buf[n], id->replyAck, aa);
        n += aa;
    }

    /* Only ever put on the air what parseNumber() would read back. */
    if (!parseNumber(buf, n, &check) || (n >= len))
        return 0;

    memcpy(out, buf, n);
    out[n] = '\0';
    return n;
}

size_t aprsMsgFormatId(char *out, size_t len, const char *addressee,
                       const char *text, const struct aprsMsgId *id)
{
    if ((out == NULL) || (len == 0) || (addressee == NULL) || (text == NULL))
        return 0;

    size_t addrLen = strlen(addressee);
    size_t textLen = strlen(text);

    if ((addrLen == 0) || (addrLen > APRS_MSG_ADDRESSEE_LEN))
        return 0;
    if (textLen > APRS_MSG_TEXT_MAX)
        return 0;

    char number[(2 * APRS_MSG_ID_MAX) + 2] = "";
    size_t numLen = 0;
    if ((id != NULL) && (id->number[0] != '\0')) {
        numLen = formatNumber(number, sizeof(number), id);
        if (numLen == 0)
            return 0;
    }

    /* ':' + nine addressee characters + ':' + text + "{number" + NUL. */
    const size_t total = MSG_TEXT_OFFSET + textLen
                       + ((numLen > 0) ? (1 + numLen) : 0);
    if (len < (total + 1))
        return 0;

    out[0] = ':';
    memcpy(&out[1], addressee, addrLen);
    for (size_t i = addrLen; i < APRS_MSG_ADDRESSEE_LEN; i++)
        out[1 + i] = ' ';
    out[MSG_TEXT_OFFSET - 1] = ':';

    memcpy(&out[MSG_TEXT_OFFSET], text, textLen);
    if (numLen > 0) {
        out[MSG_TEXT_OFFSET + textLen] = '{';
        memcpy(&out[MSG_TEXT_OFFSET + textLen + 1], number, numLen);
    }
    out[total] = '\0';

    return total;
}

size_t aprsAckFormat(char *out, size_t len, const char *addressee,
                     enum aprsMsgKind kind, const struct aprsMsgId *id)
{
    if ((id == NULL) || ((kind != APRS_MSG_ACK) && (kind != APRS_MSG_REJ)))
        return 0;

    char text[3 + (2 * APRS_MSG_ID_MAX) + 2];
    memcpy(text, (kind == APRS_MSG_ACK) ? "ack" : "rej", 3);
    if (formatNumber(&text[3], sizeof(text) - 3, id) == 0)
        return 0;

    return aprsMsgFormatId(out, len, addressee, text, NULL);
}

/* An uncompressed coordinate is "DDMM.hh" plus a hemisphere character: the
 * degrees, the minutes, and hundredths of a minute (about 18.5 m). */
#define POS_LAT_MAX_MICRO 90000000
#define POS_LON_MAX_MICRO 180000000

/* Longest each optional extension can be once formatted. */
#define POS_CS_LEN 7  /* "ddd/sss"   */
#define POS_ALT_LEN 9 /* "/A=aaaaaa" */

/**
 * Convert a coordinate in millionths of a degree to the degrees, whole
 * minutes and hundredths of a minute that APRS puts on the air.
 *
 * The fraction is scaled as frac*6/1000, not frac*6000/1000000: the latter
 * overflows a 32-bit integer near a whole degree. Rounding to the nearest
 * hundredth of a minute can push the value up to a full 60.00', which must
 * carry into the degrees rather than print as an impossible minute count.
 */
static void toMinutes(int32_t micro, uint32_t *deg, uint32_t *min,
                      uint32_t *hundredths)
{
    uint32_t u = (micro < 0) ? (uint32_t)(-micro) : (uint32_t)micro;

    *deg = u / 1000000u;

    uint32_t frac = u % 1000000u;
    uint32_t mh = (frac * 6u + 500u) / 1000u;

    if (mh >= 6000u) {
        mh -= 6000u;
        *deg += 1u;
    }

    *min = mh / 100u;
    *hundredths = mh % 100u;
}

size_t aprsPosFormat(char *out, size_t len, const struct aprsPosReport *rpt)
{
    if ((out == NULL) || (rpt == NULL))
        return 0;
    if ((rpt->latitude < -POS_LAT_MAX_MICRO)
        || (rpt->latitude > POS_LAT_MAX_MICRO)
        || (rpt->longitude < -POS_LON_MAX_MICRO)
        || (rpt->longitude > POS_LON_MAX_MICRO))
        return 0;

    uint32_t latDeg, latMin, latHun, lonDeg, lonMin, lonHun;
    toMinutes(rpt->latitude, &latDeg, &latMin, &latHun);
    toMinutes(rpt->longitude, &lonDeg, &lonMin, &lonHun);

    const char latHemi = (rpt->latitude < 0) ? 'S' : 'N';
    const char lonHemi = (rpt->longitude < 0) ? 'W' : 'E';

    /* "=DDMM.hhN<tbl>DDDMM.hhW<code>": the coordinate, symbol table and code.
     * Always present, always exactly this long, so a short buffer is rejected
     * rather than producing a coordinate missing its symbol. */
    char coord[21];
    int n = snprintf(coord, sizeof(coord),
                     "=%02u%02u.%02u%c%c%03u%02u.%02u%c%c", (unsigned)latDeg,
                     (unsigned)latMin, (unsigned)latHun, latHemi,
                     rpt->symbolTable, (unsigned)lonDeg, (unsigned)lonMin,
                     (unsigned)lonHun, lonHemi, rpt->symbolCode);
    if ((n != 20) || (len < (size_t)n + 1))
        return 0;

    memcpy(out, coord, (size_t)n);
    size_t pos = (size_t)n;

    /* Course and speed, if asked for and there is room for the whole field.
     * Speed is transmitted in knots; course wraps 360 back to zero. */
    if (rpt->hasCourseSpeed && ((pos + POS_CS_LEN) < len)) {
        unsigned course = (rpt->course >= 360u) ? 0u : rpt->course;
        unsigned knots = ((unsigned)rpt->speed * 1000u + 926u) / 1852u;
        if (knots > 999u)
            knots = 999u;

        pos += (size_t)snprintf(&out[pos], len - pos, "%03u/%03u", course,
                                knots);
    }

    /* Altitude, if asked for and it fits, in whole feet. */
    if (rpt->hasAltitude && ((pos + POS_ALT_LEN) < len)) {
        unsigned feet = 0u;
        if (rpt->altitude > 0)
            feet = ((unsigned)rpt->altitude * 10000u + 1524u) / 3048u;
        if (feet > 999999u)
            feet = 999999u;

        pos += (size_t)snprintf(&out[pos], len - pos, "/A=%06u", feet);
    }

    /* Comment last, truncated to whatever room is left: it is the least
     * important part of the report and the only field that survives partial. */
    if (rpt->comment != NULL) {
        size_t room = (len - 1) - pos;
        size_t clen = strlen(rpt->comment);
        if (clen > room)
            clen = room;
        memcpy(&out[pos], rpt->comment, clen);
        pos += clen;
    }

    out[pos] = '\0';
    return pos;
}

void aprsSanitize(char *s)
{
    if (s == NULL)
        return;

    for (; *s != '\0'; s++) {
        if (((unsigned char)*s < 0x20) || (*s == 0x7f))
            *s = '.';
    }
}

/* Weather fields: a letter, then a fixed number of digits. */
static const struct {
    char key;
    uint8_t width;
    uint16_t bit;
} wxFields[] = {
    { 'c', 3, APRS_WX_WIND_DIR },   { 's', 3, APRS_WX_WIND_SPEED },
    { 'g', 3, APRS_WX_GUST },       { 't', 3, APRS_WX_TEMP },
    { 'r', 3, APRS_WX_RAIN_1H },    { 'p', 3, APRS_WX_RAIN_24H },
    { 'P', 3, APRS_WX_RAIN_TODAY }, { 'h', 2, APRS_WX_HUMIDITY },
    { 'b', 5, APRS_WX_PRESSURE },
};

static void wxStore(struct aprsWeather *wx, uint16_t bit, long v)
{
    switch (bit) {
        case APRS_WX_WIND_DIR:
            wx->windDir = (uint16_t)(v % 360);
            break;
        case APRS_WX_WIND_SPEED:
            wx->windSpeed = (uint16_t)v;
            break;
        case APRS_WX_GUST:
            wx->gust = (uint16_t)v;
            break;
        case APRS_WX_TEMP:
            wx->temp = (int16_t)v;
            break;
        case APRS_WX_RAIN_1H:
            wx->rain1h = (uint16_t)v;
            break;
        case APRS_WX_RAIN_24H:
            wx->rain24h = (uint16_t)v;
            break;
        case APRS_WX_RAIN_TODAY:
            wx->rainToday = (uint16_t)v;
            break;
        case APRS_WX_HUMIDITY:
            wx->humidity = (uint8_t)((v == 0) ? 100 : v); /* "h00" is 100% */
            break;
        case APRS_WX_PRESSURE:
            wx->pressure = (uint16_t)v;
            break;
    }
    wx->valid |= bit;
}

/* Read weather fields until one is not recognised. */
static void wxParseFields(const char *s, size_t len, struct aprsWeather *wx)
{
    size_t i = 0;

    while (i < len) {
        size_t f = 0;
        while ((f < sizeof(wxFields) / sizeof(wxFields[0]))
               && (wxFields[f].key != s[i]))
            f++;
        if (f == sizeof(wxFields) / sizeof(wxFields[0]))
            return;

        size_t w = wxFields[f].width;
        if (i + 1 + w > len)
            return;

        const char *d = &s[i + 1];
        bool neg = (d[0] == '-');
        bool digits = true;
        bool missing = true;
        long v = 0;
        for (size_t j = neg ? 1 : 0; j < w; j++) {
            digits = digits && isDigit(d[j]);
            missing = missing && ((d[j] == '.') || (d[j] == ' '));
            v = (v * 10) + (isDigit(d[j]) ? (d[j] - '0') : 0);
        }
        if (digits)
            wxStore(wx, wxFields[f].bit, neg ? -v : v);
        else if (!missing)
            return;

        i += 1 + w;
    }
}

bool aprsWeatherParse(const struct aprsPacket *pkt, struct aprsWeather *wx)
{
    if ((pkt == NULL) || (wx == NULL) || (pkt->infoLen == 0))
        return false;

    struct aprsWeather w;
    memset(&w, 0, sizeof(w));

    if (pkt->info[0] == '_') {
        /* "_MMDDHHMM" then the fields, wind first as c and s */
        if (pkt->infoLen < 10)
            return false;
        wxParseFields(&pkt->info[9], pkt->infoLen - 9, &w);
    } else {
        struct aprsPosition pos;
        if (!aprsPosParse(pkt, &pos) || (pos.symbolCode != '_'))
            return false;

        /* The course/speed slot just before the comment is the wind. */
        const char *c = pos.comment;
        if ((c - pkt->info >= 7) && (c[-4] == '/')) {
            char slot[8];
            memcpy(slot, c - 7, 7);
            slot[7] = '\0';
            char fields[10] = { 'c',     slot[0], slot[1], slot[2], 's',
                                slot[4], slot[5], slot[6], '\0' };
            wxParseFields(fields, 8, &w);
        }
        wxParseFields(c, pos.commentLen, &w);
    }

    if (w.valid == 0)
        return false;

    *wx = w;
    return true;
}

size_t aprsFrameToTnc2(const uint8_t *frame, size_t len, char *out, size_t cap)
{
    if ((frame == NULL) || (out == NULL) || (cap == 0) || (len > APRS_PACLEN))
        return 0;

    size_t count = countAddresses(frame, len);
    size_t info = (size_t)count * APRS_ADDR_LEN + APRS_CTRL_PID_LEN;
    if ((count == 0) || (info > len))
        return 0;

    uint8_t lastUsed = 0;
    for (uint8_t i = 2; i < count; i++) {
        if (frame[i * APRS_ADDR_LEN + APRS_ADDR_LEN - 1] & ADDR_CMD_OR_H)
            lastUsed = i;
    }

    /* Source first, then destination, then the path */
    size_t pos = 0;
    for (uint8_t k = 0; k < count; k++) {
        uint8_t i = (k == 0) ? 1 : ((k == 1) ? 0 : k);
        struct aprsAddress addr;
        char call[APRS_ADDR_STR_LEN];

        memset(&addr, 0, sizeof(addr));
        parseAddress(&frame[i * APRS_ADDR_LEN], &addr);
        size_t n = aprsAddr_toStr(&addr, call, sizeof(call));
        const char *sep = (k == 0) ? "" : ((k == 1) ? ">" : ",");
        const char *mark = ((i >= 2) && (i == lastUsed)) ? "*" : "";

        size_t need = strlen(sep) + n + strlen(mark);
        if (pos + need >= cap)
            return 0;
        pos += (size_t)snprintf(&out[pos], cap - pos, "%s%s%s", sep, call,
                                mark);
    }

    size_t infoLen = len - info;
    if (pos + 1 + infoLen >= cap)
        return 0;
    out[pos++] = ':';
    memcpy(&out[pos], &frame[info], infoLen);
    pos += infoLen;
    out[pos] = '\0';

    return pos;
}

size_t aprsFrameFromTnc2(const char *line, size_t len, uint8_t *buf, size_t cap)
{
    char header[APRS_MAX_ADDRESSES * APRS_ADDR_STR_LEN];

    if ((line == NULL) || (buf == NULL))
        return 0;

    /* No address contains a colon: the first one ends the header. */
    const char *colon = memchr(line, ':', len);
    if ((colon == NULL) || ((size_t)(colon - line) >= sizeof(header)))
        return 0;

    size_t hlen = (size_t)(colon - line);
    memcpy(header, line, hlen);
    header[hlen] = '\0';

    char *gt = strchr(header, '>');
    if ((gt == NULL) || (gt == header))
        return 0;
    *gt = '\0';

    const char *src = header;
    char *dest = gt + 1;
    char *path = strchr(dest, ',');
    if (path != NULL)
        *path++ = '\0';

    const char *info = colon + 1;
    size_t infoLen = len - hlen - 1;
    if (infoLen == 0)
        return 0;

    const ssize_t n = aprsFrame_build(buf, cap, dest, src, path, info, infoLen);
    return (n > 0) ? (size_t)n : 0;
}

bool aprsGroupAddressee(const char *addressee)
{
    if (addressee == NULL)
        return false;

    if ((strncmp(addressee, "BLN", 3) == 0)
        || (strncmp(addressee, "NWS-", 4) == 0)
        || (strncmp(addressee, "SKY", 3) == 0))
        return true;

    return (strcmp(addressee, "ALL") == 0) || (strcmp(addressee, "CQ") == 0)
        || (strcmp(addressee, "QST") == 0);
}

bool aprsEmergency(const struct aprsPacket *pkt)
{
    if (pkt == NULL)
        return false;

    static const char word[] = "EMERGENCY";
    const size_t wordLen = sizeof(word) - 1;
    for (size_t i = 0; (i + wordLen) <= pkt->infoLen; i++) {
        if (memcmp(&pkt->info[i], word, wordLen) == 0)
            return true;
    }

    /* Only Mic-E carries the Emergency comment in its bits. */
    char dti = pkt->info[0];
    if ((dti != 0x1c) && (dti != 0x1d) && (dti != '`') && (dti != '\''))
        return false;

    struct aprsPosition pos;
    return aprsPosParse(pkt, &pos) && pos.emergency;
}

enum aprsKind aprsPacketKind(const struct aprsPacket *pkt, const char *me)
{
    if (pkt == NULL)
        return APRS_KIND_OTHER;

    if (aprsEmergency(pkt))
        return APRS_KIND_EMERGENCY;

    switch (pkt->info[0]) {
        case '}':
            return APRS_KIND_THIRD_PARTY;
        case '?':
            return APRS_KIND_QUERY;
        default:
            break;
    }

    if (pkt->type == APRS_TYPE_MESSAGE) {
        char addressee[APRS_ADDR_STR_LEN];
        char text[APRS_PACLEN];
        if (aprsMsg_unwrap(pkt, addressee, sizeof(addressee), text,
                           sizeof(text))
            != 0)
            return APRS_KIND_OTHER;

        struct aprsMsgId id;
        if (aprsMsgParseId(pkt, &id) && (id.kind != APRS_MSG_TEXT))
            return APRS_KIND_ACK;
        if (text[0] == '?')
            return APRS_KIND_QUERY;
        if (aprsGroupAddressee(addressee))
            return APRS_KIND_GROUP;

        struct aprsAddress a, b;
        if ((me != NULL) && (aprsAddr_fromStr(addressee, &a) == 0)
            && (aprsAddr_fromStr(me, &b) == 0) && (strcmp(a.addr, b.addr) == 0)
            && (a.ssid == b.ssid))
            return APRS_KIND_TO_ME;

        return APRS_KIND_MESSAGE;
    }

    if ((pkt->type == APRS_TYPE_POSITION) || (pkt->type == APRS_TYPE_OBJECT))
        return APRS_KIND_POSITION;

    return APRS_KIND_OTHER;
}

bool aprsPathValid(const char *path)
{
    if (path == NULL)
        return false;
    if (path[0] == '\0')
        return true;

    unsigned hops = 0;
    const char *p = path;
    while (true) {
        const char *comma = strchr(p, ',');
        size_t len = (comma != NULL) ? (size_t)(comma - p) : strlen(p);
        char hop[APRS_ADDR_STR_LEN];

        if ((len == 0) || (len >= sizeof(hop)) || (++hops > 8))
            return false;
        memcpy(hop, p, len);
        hop[len] = '\0';
        if (aprsAddr_validate(hop) != 0)
            return false;

        if (comma == NULL)
            return true;
        p = comma + 1;
    }
}

/* ------------------------------------------------------------------------ *
 * Position parsing — the inverse of aprsPosFormat().
 * ------------------------------------------------------------------------ */

static bool isDigit(char c)
{
    return (c >= '0') && (c <= '9');
}

/**
 * Read "DDMM.hh" or "DDDMM.hh" into micro-degrees. A space stands for a
 * withheld digit, read as zero.
 */
static bool parseMinutes(const char *s, size_t degDigits, int32_t *micro,
                         bool *ambiguous)
{
    uint32_t deg = 0;
    for (size_t i = 0; i < degDigits; i++) {
        if (!isDigit(s[i]))
            return false;
        deg = (deg * 10u) + (uint32_t)(s[i] - '0');
    }

    /* MM.hh; once a digit is withheld the rest must be too */
    uint32_t mmhh = 0;
    bool blank = false;
    const size_t at[4] = { degDigits, degDigits + 1, degDigits + 3,
                           degDigits + 4 };
    if (s[degDigits + 2] != '.')
        return false;
    for (size_t i = 0; i < 4; i++) {
        char c = s[at[i]];
        if (c == ' ')
            blank = true;
        else if (!isDigit(c) || blank)
            return false;
        mmhh = (mmhh * 10u) + (uint32_t)(blank ? 0 : (c - '0'));
    }
    if (blank)
        *ambiguous = true;

    if ((mmhh / 100u) >= 60u)
        return false;

    *micro =
        (int32_t)((deg * 1000000u) + (((mmhh * 1000000u) + 3000u) / 6000u));
    return true;
}

/** Uncompressed "DDMM.hhN<t>DDDMM.hhW<c>", 19 characters. */
static bool parseUncompressed(const char *s, size_t len,
                              struct aprsPosition *pos)
{
    if (len < 19)
        return false;

    int32_t lat, lon;
    if (!parseMinutes(s, 2, &lat, &pos->ambiguous))
        return false;
    if (!parseMinutes(&s[9], 3, &lon, &pos->ambiguous))
        return false;

    if (s[7] == 'S')
        lat = -lat;
    else if (s[7] != 'N')
        return false;
    if (s[17] == 'W')
        lon = -lon;
    else if (s[17] != 'E')
        return false;

    if ((lat > POS_LAT_MAX_MICRO) || (lat < -POS_LAT_MAX_MICRO)
        || (lon > POS_LON_MAX_MICRO) || (lon < -POS_LON_MAX_MICRO))
        return false;

    pos->latitude = lat;
    pos->longitude = lon;
    pos->symbolTable = s[8];
    pos->symbolCode = s[18];
    return true;
}

/** Four base-91 characters. */
static bool base91(const char *s, uint32_t *out)
{
    uint32_t n = 0;
    for (int i = 0; i < 4; i++) {
        if ((s[i] < '!') || (s[i] > '{'))
            return false;
        n = (n * 91u) + (uint32_t)(s[i] - '!');
    }
    *out = n;
    return true;
}

/** Compressed "<t>YYYYXXXX<c>csT", 13 characters. */
static bool parseCompressed(const char *s, size_t len, struct aprsPosition *pos)
{
    if (len < 13)
        return false;

    uint32_t y, x;
    if (!base91(&s[1], &y) || !base91(&s[5], &x))
        return false;

    pos->latitude = (int32_t)lround((90.0 - (y / 380926.0)) * 1000000.0);
    pos->longitude = (int32_t)lround((-180.0 + (x / 190463.0)) * 1000000.0);
    pos->symbolTable = s[0];
    pos->symbolCode = s[9];

    const char c = s[10], sp = s[11], t = s[12];
    if ((c == ' ') || (t < '!'))
        return true;

    if (((t - '!') & 0x18) == 0x10) {
        /* Altitude: 1.002^(c*91+s) feet */
        double feet = pow(1.002, (double)(((c - '!') * 91) + (sp - '!')));
        pos->hasAltitude = true;
        pos->altitude = (int16_t)lround(feet * 0.3048);
    } else if ((c >= '!') && (c <= 'z')) {
        /* Course in 4 degree steps, speed 1.08^s - 1 knots */
        double knots = pow(1.08, (double)(sp - '!')) - 1.0;
        pos->hasCourseSpeed = true;
        pos->course = (uint16_t)(((c - '!') * 4) % 360);
        pos->speed = (uint16_t)lround(knots * 1.852);
    }

    return true;
}

/**
 * A "!DAO!" anywhere in the comment adds a third decimal of minutes: 'W'
 * as two digits, 'w' as two base-91 characters.
 */
static void applyDao(const char *s, size_t len, struct aprsPosition *pos)
{
    for (size_t i = 0; i + 5 <= len; i++) {
        if ((s[i] != '!') || (s[i + 4] != '!'))
            continue;

        char d = s[i + 1];
        char a = s[i + 2];
        char o = s[i + 3];
        int32_t dlat, dlon;

        if ((d == 'W') && isDigit(a) && isDigit(o)) {
            /* thousandths of a minute: 1e6 / 60 / 1000 microdegrees */
            dlat = ((a - '0') * 1000000 + 30000) / 60000;
            dlon = ((o - '0') * 1000000 + 30000) / 60000;
        } else if ((d == 'w') && (a >= '!') && (a <= '{') && (o >= '!')
                   && (o <= '{')) {
            /* (c - 33) / 91 hundredths of a minute */
            dlat = ((a - 33) * 10000000 + 2730000) / 5460000;
            dlon = ((o - 33) * 10000000 + 2730000) / 5460000;
        } else {
            continue;
        }

        pos->latitude += (pos->latitude < 0) ? -dlat : dlat;
        pos->longitude += (pos->longitude < 0) ? -dlon : dlon;
        pos->hasDao = true;
        return;
    }
}

/**
 * A "PHGphgd" or "RNGrrrr" data extension in the seven bytes after a
 * position.
 */
static bool parseDataExtension(const char *s, size_t len,
                               struct aprsPosition *pos)
{
    if (len < 7)
        return false;

    if ((memcmp(s, "PHG", 3) == 0) && isDigit(s[3]) && isDigit(s[4])
        && isDigit(s[5]) && isDigit(s[6])) {
        unsigned p = (unsigned)(s[3] - '0');
        pos->hasPhg = true;
        pos->phgPower = (uint16_t)(p * p);
        pos->phgHeight = (uint16_t)(10u << (s[4] - '0'));
        pos->phgGain = (uint8_t)(s[5] - '0');
        pos->phgDir = (uint16_t)((s[6] - '0') * 45);
        return true;
    }

    if ((memcmp(s, "RNG", 3) == 0) && isDigit(s[3]) && isDigit(s[4])
        && isDigit(s[5]) && isDigit(s[6])) {
        pos->range = (uint16_t)((s[3] - '0') * 1000 + (s[4] - '0') * 100
                                + (s[5] - '0') * 10 + (s[6] - '0'));
        return true;
    }

    return false;
}

/**
 * The fields after a position: an optional "ddd/sss" course and speed, an
 * "/A=aaaaaa" altitude anywhere, and the comment.
 */
static void parseExtensions(const char *s, size_t len, struct aprsPosition *pos)
{
    size_t at = 0;

    if (parseDataExtension(s, len, pos)) {
        at = 7;
    } else if ((len >= 7) && (s[3] == '/')) {
        bool digits = true;
        bool unknown = true;
        for (size_t i = 0; i < 7; i++) {
            if (i == 3)
                continue;
            digits = digits && (isDigit(s[i]) || (s[i] == '.'));
            unknown = unknown && ((s[i] == '0') || (s[i] == '.'));
        }
        if (digits) {
            at = 7;
            if (!unknown && isDigit(s[0])) {
                unsigned course = (unsigned)((s[0] - '0') * 100
                                             + (s[1] - '0') * 10
                                             + (s[2] - '0'));
                unsigned knots = (unsigned)((s[4] - '0') * 100
                                            + (s[5] - '0') * 10 + (s[6] - '0'));
                pos->hasCourseSpeed = true;
                pos->course = (uint16_t)(course % 360);
                pos->speed = (uint16_t)((knots * 1852u + 500u) / 1000u);
            }
        }
    }

    for (size_t i = at; i + 9 <= len; i++) {
        if ((s[i] != '/') || (s[i + 1] != 'A') || (s[i + 2] != '='))
            continue;
        const char *d = &s[i + 3];
        bool neg = (d[0] == '-');
        bool ok = true;
        long feet = 0;
        for (size_t j = neg ? 1 : 0; j < 6; j++) {
            ok = ok && isDigit(d[j]);
            feet = (feet * 10) + (ok ? (d[j] - '0') : 0);
        }
        if (ok) {
            pos->hasAltitude = true;
            pos->altitude = (int16_t)lround((neg ? -feet : feet) * 0.3048);
            break;
        }
    }

    applyDao(s, len, pos);
    pos->comment = &s[at];
    pos->commentLen = (uint16_t)((at <= len) ? (len - at) : 0);
}

/**
 * Mic-E: the latitude, the N/S, W/E and longitude-offset flags and the
 * position comment come from the destination address; the longitude,
 * speed, course and symbol follow in the info field, each byte offset by
 * 28. APRS 1.2 chapter 10.
 */
static bool parseMicE(const struct aprsPacket *pkt, struct aprsPosition *p)
{
    const char *dest = pkt->addresses[0].addr;
    const char *s = pkt->info;
    size_t len = pkt->infoLen;

    if ((strlen(dest) != 6) || (len < 9))
        return false;

    uint32_t digits[6];
    bool north = false, west = false, offset = false;

    for (int i = 0; i < 6; i++) {
        char c = dest[i];
        bool high; /* the North / +100 / West family of characters */

        if ((c >= '0') && (c <= '9')) {
            digits[i] = (uint32_t)(c - '0');
            high = false;
        } else if ((c >= 'A') && (c <= 'J')) {
            digits[i] = (uint32_t)(c - 'A');
            high = false;
        } else if ((c == 'K') || (c == 'L') || (c == 'Z')) {
            digits[i] = 0;
            p->ambiguous = true;
            high = (c != 'L');
        } else if ((c >= 'P') && (c <= 'Y')) {
            digits[i] = (uint32_t)(c - 'P');
            high = true;
        } else {
            return false;
        }

        if (i == 3)
            north = high;
        else if (i == 4)
            offset = high;
        else if (i == 5)
            west = high;
    }

    /* The first three bytes carry the position comment bits, set by a
     * letter; all three clear is the Emergency comment. */
    p->emergency = true;
    for (int i = 0; i < 3; i++) {
        if ((dest[i] < '0') || (dest[i] > '9'))
            p->emergency = false;
    }

    uint32_t mmhh = (digits[2] * 1000u) + (digits[3] * 100u) + (digits[4] * 10u)
                  + digits[5];
    if (((digits[0] * 10u) + digits[1] > 90u) || ((mmhh / 100u) >= 60u))
        return false;
    int32_t lat = (int32_t)(((digits[0] * 10u) + digits[1]) * 1000000u
                            + (((mmhh * 1000000u) + 3000u) / 6000u));
    p->latitude = north ? lat : -lat;

    int d = (int)(uint8_t)s[1] - 28;
    int m = (int)(uint8_t)s[2] - 28;
    int h = (int)(uint8_t)s[3] - 28;
    if (offset)
        d += 100;
    if ((d >= 180) && (d <= 189))
        d -= 80;
    else if ((d >= 190) && (d <= 199))
        d -= 190;
    if (m >= 60)
        m -= 60;
    if ((d < 0) || (d > 180) || (m < 0) || (m >= 60) || (h < 0) || (h > 99))
        return false;
    if (p->ambiguous)
        h = 0;
    int32_t lon =
        (int32_t)((uint32_t)d * 1000000u
                  + ((((uint32_t)m * 100u + (uint32_t)h) * 1000000u + 3000u)
                     / 6000u));
    p->longitude = west ? -lon : lon;

    int sp = (int)(uint8_t)s[4] - 28;
    int dc = (int)(uint8_t)s[5] - 28;
    int se = (int)(uint8_t)s[6] - 28;
    if ((sp < 0) || (dc < 0) || (se < 0))
        return false;
    int knots = (sp * 10) + (dc / 10);
    int course = ((dc % 10) * 100) + se;
    if (knots >= 800)
        knots -= 800;
    if (course >= 400)
        course -= 400;
    if ((knots > 0) || (course > 0)) {
        p->hasCourseSpeed = true;
        p->course = (uint16_t)(course % 360);
        p->speed = (uint16_t)(((unsigned)knots * 1852u + 500u) / 1000u);
    }

    p->symbolCode = s[7];
    p->symbolTable = s[8];

    /* An altitude is "xxx}" right after the symbol: base-91 metres + 10000 */
    size_t at = 9;
    if ((len >= 13) && (s[12] == '}')) {
        uint32_t n = 0;
        bool ok = true;
        for (size_t i = 9; i < 12; i++) {
            ok = ok && (s[i] >= '!') && (s[i] <= '{');
            n = (n * 91u) + (uint32_t)(s[i] - '!');
        }
        if (ok) {
            p->hasAltitude = true;
            p->altitude = (int16_t)((int32_t)n - 10000);
            at = 13;
        }
    }

    p->comment = &s[at];
    p->commentLen = (uint16_t)(len - at);
    applyDao(p->comment, p->commentLen, p);
    return true;
}

bool aprsPosParse(const struct aprsPacket *pkt, struct aprsPosition *pos)
{
    if ((pkt == NULL) || (pos == NULL) || (pkt->infoLen == 0))
        return false;

    struct aprsPosition p;
    memset(&p, 0, sizeof(p));

    const char *s = pkt->info;
    size_t len = pkt->infoLen;
    size_t at = 1;

    switch (s[0]) {
        case '!':
        case '=':
            break;

        case '/':
        case '@':
            at += 7; /* timestamp */
            break;

        case '`':
        case '\'':
        case 0x1c:
        case 0x1d:
            if (!parseMicE(pkt, &p))
                return false;
            *pos = p;
            return true;

        case ';': /* ";NAME     *DDHHMMz<position>" */
            if (len < 18)
                return false;
            memcpy(p.name, &s[1], 9);
            p.name[9] = '\0';
            for (int i = 8; (i >= 0) && (p.name[i] == ' '); i--)
                p.name[i] = '\0';
            if ((s[10] != '*') && (s[10] != '_'))
                return false;
            p.killed = (s[10] == '_');
            at = 18;
            break;

        case ')': { /* ")NAME!<position>" with a 3 to 9 character name */
            size_t n = 1;
            while ((n < len) && (n <= 9) && (s[n] != '!') && (s[n] != '_'))
                n++;
            if ((n < 4) || (n >= len))
                return false;
            memcpy(p.name, &s[1], n - 1);
            p.name[n - 1] = '\0';
            p.killed = (s[n] == '_');
            at = n + 1;
            break;
        }

        default:
            return false;
    }

    if (at >= len)
        return false;

    size_t used;
    if (isDigit(s[at]) || (s[at] == ' ')) {
        if (!parseUncompressed(&s[at], len - at, &p))
            return false;
        used = 19;
    } else {
        if (!parseCompressed(&s[at], len - at, &p))
            return false;
        used = 13;
    }

    parseExtensions(&s[at + used], len - at - used, &p);

    *pos = p;
    return true;
}

/* ------------------------------------------------------------------------ *
 * Frequency announcements — aprs.org/info/freqspec.txt.
 * ------------------------------------------------------------------------ */

static char upperOf(char c)
{
    return ((c >= 'a') && (c <= 'z')) ? (char)(c - 'a' + 'A') : c;
}

/* Case-insensitive match of the @p n characters at @p w against @p word. */
static bool wordIs(const char *w, size_t n, const char *word)
{
    if (strlen(word) != n)
        return false;

    for (size_t i = 0; i < n; i++) {
        if (upperOf(w[i]) != word[i])
            return false;
    }

    return true;
}

/**
 * Read "146.82" or "441.112" into Hz; return the characters read, 0 if none.
 * The 1 kHz the spec allows cuts 12.5 kHz channels short: 441.112 is read
 * as 441.1125.
 */
static size_t readMhz(const char *s, uint32_t *hz)
{
    uint32_t mhz = 0;
    size_t i = 0;
    while ((i < 3) && isDigit(s[i]))
        mhz = (mhz * 10) + (uint32_t)(s[i++] - '0');
    if ((i < 2) || (s[i] != '.'))
        return 0;
    i++;

    uint32_t frac = 0;
    size_t digits = 0;
    while ((digits < 4) && isDigit(s[i])) {
        frac = (frac * 10) + (uint32_t)(s[i++] - '0');
        digits++;
    }
    if (digits < 2)
        return 0;

    for (size_t d = digits; d < 6; d++)
        frac *= 10;
    if ((digits == 3) && (((frac / 1000) % 25) == 12))
        frac += 500;

    *hz = (mhz * 1000000u) + frac;
    return i;
}

/* "Txxx", "Cxxx", "Dxxx", "1750" or "Toff"; lower case is narrow FM. */
static bool readTone(const char *w, size_t n, struct aprsFreq *f)
{
    if ((n == 4) && wordIs(&w[1], 3, "OFF") && (upperOf(w[0]) == 'T')) {
        f->toneType = APRS_TONE_NONE;
        f->offset = 0;
        f->flags |= APRS_FREQ_HAS_OFFSET;
        return true;
    }

    if ((n != 4) || !isDigit(w[1]) || !isDigit(w[2]) || !isDigit(w[3]))
        return false;

    const bool narrow = (w[0] >= 'a') && (w[0] <= 'z');
    const uint16_t v =
        (uint16_t)(((w[1] - '0') * 100) + ((w[2] - '0') * 10) + (w[3] - '0'));
    switch (upperOf(w[0])) {
        case 'T':
            f->toneType = APRS_TONE_ENCODE;
            break;
        case 'C':
            f->toneType = APRS_TONE_SQUELCH;
            break;
        case 'D':
            f->toneType = APRS_TONE_DCS;
            break;
        case '1':
        case 'L':
            if (v != 750)
                return false;
            f->toneType = APRS_TONE_BURST;
            break;
        default:
            return false;
    }

    f->tone = (f->toneType == APRS_TONE_BURST) ? 1750 : v;
    if (narrow)
        f->flags |= APRS_FREQ_NARROW;
    return true;
}

/* "+060" in 10 kHz, or "FFF.FFFrx" where a split repeater listens. */
static bool readOffset(const char *w, size_t n, struct aprsFreq *f)
{
    if ((n == 4) && ((w[0] == '+') || (w[0] == '-')) && isDigit(w[1])
        && isDigit(w[2]) && isDigit(w[3])) {
        int32_t v = ((w[1] - '0') * 100) + ((w[2] - '0') * 10) + (w[3] - '0');
        f->offset = ((w[0] == '-') ? -v : v) * 10000;
        f->flags |= APRS_FREQ_HAS_OFFSET;
        return true;
    }

    uint32_t rx;
    size_t used = readMhz(w, &rx);
    if ((used > 0) && (used + 2 == n) && wordIs(&w[used], 2, "RX")) {
        f->offset = (int32_t)(rx - f->freq);
        f->flags |= APRS_FREQ_HAS_OFFSET;
        return true;
    }

    return false;
}

/* "R25m", "R40k", or "E25m" and its kin for ranges by direction. */
static bool isRange(const char *w, size_t n)
{
    if ((n < 3) || (strchr("RNSEW", w[0]) == NULL)
        || ((w[n - 1] != 'm') && (w[n - 1] != 'k')))
        return false;

    for (size_t i = 1; i < n - 1; i++) {
        if (!isDigit(w[i]))
            return false;
    }

    return true;
}

/* "CAN0", "CAN=3"; or 0-15 after a lone "CAN". */
static bool readCan(const char *w, size_t n, bool afterCan, struct aprsFreq *f)
{
    size_t i = 0;
    if (!afterCan) {
        if ((n < 4) || !wordIs(w, 3, "CAN"))
            return false;
        i = 3;
        if ((w[i] == '=') || (w[i] == ':'))
            i++;
    }

    if ((i == n) || (n - i > 2))
        return false;

    unsigned v = 0;
    for (; i < n; i++) {
        if (!isDigit(w[i]))
            return false;
        v = (v * 10) + (unsigned)(w[i] - '0');
    }
    if (v > 15)
        return false;

    f->can = (uint8_t)v;
    f->flags |= APRS_FREQ_HAS_CAN;
    return true;
}

/* Kenwood's Mic-E ends the comment with '=' or '^' for the radio type */
static bool isSeparator(char c)
{
    return (c != '\0') && (strchr(" /,;()=^", c) != NULL);
}

/**
 * The fields after the frequency. Tone, offset and range come first; the
 * first other word ends them, so free text cannot be mistaken for them.
 * Mode words count anywhere.
 */
static void readFields(const char *c, struct aprsFreq *f)
{
    bool lead = true;
    bool afterCan = false;
    bool m17 = false, data = false, fm = false;
    uint8_t digital = APRS_FREQ_FM;

    while (*c != '\0') {
        while (isSeparator(*c))
            c++;
        size_t n = 0;
        while ((c[n] != '\0') && !isSeparator(c[n]))
            n++;
        if (n == 0)
            break;
        const char *w = c;
        c += n;

        if (readCan(w, n, afterCan, f)) {
            afterCan = false;
            continue;
        }
        afterCan = wordIs(w, n, "CAN");
        if (afterCan)
            continue;

        if (lead && (readTone(w, n, f) || readOffset(w, n, f) || isRange(w, n)))
            continue;

        /* "D-STAR>CALLSN A" names the repeater after the mode */
        const char *gt = memchr(w, '>', n);
        size_t wn = (gt != NULL) ? (size_t)(gt - w) : n;

        if (wordIs(w, wn, "M17")) {
            m17 = true;
        } else if (wordIs(w, wn, "VARA") || wordIs(w, wn, "PACKET")
                   || wordIs(w, wn, "WINLINK") || wordIs(w, wn, "ARDOP")) {
            data = true;
        } else if (wordIs(w, wn, "FM") || wordIs(w, wn, "ANALOG")
                   || wordIs(w, wn, "ALLSTAR") || wordIs(w, wn, "ECHOLINK")
                   || wordIs(w, wn, "IRLP")) {
            fm = true;
        } else if (wordIs(w, wn, "DMR") || wordIs(w, wn, "MOTOTRBO")) {
            if (digital == APRS_FREQ_FM)
                digital = APRS_FREQ_DMR;
        } else if (wordIs(w, wn, "D-STAR") || wordIs(w, wn, "DSTAR")
                   || wordIs(w, wn, "DV")) {
            if (digital == APRS_FREQ_FM)
                digital = APRS_FREQ_DSTAR;
        } else if (wordIs(w, wn, "FUSION") || wordIs(w, wn, "C4FM")
                   || wordIs(w, wn, "YSF")) {
            if (digital == APRS_FREQ_FM)
                digital = APRS_FREQ_FUSION;
        } else if (wordIs(w, wn, "P25")) {
            if (digital == APRS_FREQ_FM)
                digital = APRS_FREQ_P25;
        } else if (wordIs(w, wn, "NXDN")) {
            if (digital == APRS_FREQ_FM)
                digital = APRS_FREQ_NXDN;
        } else {
            lead = false;
        }
    }

    /* A mode this radio works wins; a word for FM marks a mixed repeater */
    if (m17)
        f->mode = APRS_FREQ_M17;
    else if (data)
        f->mode = APRS_FREQ_DATA;
    else if (fm)
        f->mode = APRS_FREQ_FM;
    else
        f->mode = digital;
}

bool aprsFreqParse(const struct aprsPacket *pkt, struct aprsFreq *f)
{
    struct aprsPosition pos;
    char text[APRS_MAX_INFO + 1];
    const char *c;
    uint32_t named = 0;

    if ((pkt == NULL) || (f == NULL))
        return false;

    if (aprsPosParse(pkt, &pos)) {
        size_t n = (pos.comment != NULL) ? pos.commentLen : 0;
        if (n > 0)
            memcpy(text, pos.comment, n);
        text[n] = '\0';
        c = text;
        if (pos.name[0] != '\0')
            readMhz(pos.name, &named);

        /* Mic-E: the sender's radio type, then perhaps an altitude */
        const char dti = pkt->info[0];
        if ((dti == '`') || (dti == '\'') || (dti == 0x1c) || (dti == 0x1d)) {
            if ((c[0] != '\0') && (strchr("`'>]", c[0]) != NULL))
                c++;
            if ((strlen(c) >= 4) && (c[3] == '}'))
                c += 4;
        }
    } else if (pkt->type == APRS_TYPE_STATUS) {
        memcpy(text, &pkt->info[1], pkt->infoLen - 1);
        text[pkt->infoLen - 1] = '\0';
        c = text;
        bool stamp = (strlen(c) >= 7) && (c[6] == 'z');
        for (size_t i = 0; stamp && (i < 6); i++)
            stamp = isDigit(c[i]);
        if (stamp)
            c += 7;
    } else {
        return false;
    }

    /* A leading delimiter, or an altitude, may come first */
    if ((strncmp(c, "/A=", 3) == 0) && (strlen(c) >= 9))
        c += 9;
    if ((*c == ' ') || (*c == '/'))
        c++;

    uint32_t first = 0;
    size_t used = readMhz(c, &first);
    if (used > 0) {
        const char *m = &c[used];
        if (*m == ' ')
            m++;
        if ((m[0] != '\0') && (m[1] != '\0') && wordIs(m, 3, "MHZ"))
            c = &m[3];
        else
            first = 0;
    }

    if ((named == 0) && (first == 0))
        return false;

    struct aprsFreq r;
    memset(&r, 0, sizeof(r));
    r.freq = (named != 0) ? named : first;

    /* Named for its frequency, a frequency in the comment is where it
     * listens: a split or crossband repeater. */
    if ((named != 0) && (first != 0) && (first != named)) {
        r.offset = (int32_t)(first - named);
        r.flags |= APRS_FREQ_HAS_OFFSET;
    }

    readFields(c, &r);
    *f = r;
    return true;
}
