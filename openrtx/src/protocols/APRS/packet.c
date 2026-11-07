/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/APRS/packet.h"

#include <errno.h>
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
            return APRS_TYPE_POSITION;
        case '>':
            return APRS_TYPE_STATUS;
        case ';':
        case ')':
            return APRS_TYPE_OBJECT;
        case 'T':
            return APRS_TYPE_TELEMETRY;
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
