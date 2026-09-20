/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * packet.c — AX.25 frame parsing for APRS.
 *
 * Turns the bytes a decoded frame carries into addresses and an info field.
 * Parsing writes into storage the caller owns: this code allocates nothing,
 * keeps no state between calls, and holds no reference to the frame once it
 * returns, so it is equally usable from the RTX thread and the UI thread.
 */

#include "protocols/APRS/packet.h"

#include <stdio.h>
#include <string.h>

/* An AX.25 address is seven bytes: six shifted-ASCII characters plus a flags
 * byte laid out as CRRSSSSL — command/response, two reserved bits, the
 * four-bit SSID, and the last-address marker. */
#define ADDR_LEN 7
#define ADDR_LAST 0x01
#define ADDR_SSID_MASK 0x1e
#define ADDR_SSID_SHIFT 1
#define ADDR_REPEATED 0x80

/* Control field and protocol identifier, between the addresses and the info
 * field. APRS always uses a UI frame with no layer 3 protocol. */
#define CTRL_PID_LEN 2

/**
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

/**
 * Unshift one seven-byte AX.25 address into @p addr.
 */
static void parseAddress(const uint8_t *src, struct aprsAddress *addr)
{
    uint8_t j = 0;

    for (uint8_t k = 0; k < ADDR_LEN - 1; k++) {
        /* Address characters are ASCII shifted left one bit; the padding
         * spaces this unshifts to are dropped along with any other
         * non-printable byte. */
        uint8_t c = src[k] >> 1;
        if ((c > ' ') && (c < 0x7f))
            addr->addr[j++] = (char)c;
    }

    addr->addr[j] = '\0';
    addr->ssid = (src[ADDR_LEN - 1] & ADDR_SSID_MASK) >> ADDR_SSID_SHIFT;
    addr->repeated = (src[ADDR_LEN - 1] & ADDR_REPEATED) ? 1 : 0;
}

/**
 * Count the addresses a frame carries, or 0 if its address field is
 * malformed: too short for a destination and a source, never terminated by
 * the last-address bit, or longer than AX.25 permits.
 */
static uint8_t countAddresses(const uint8_t *data, size_t len)
{
    size_t offset = 0;
    uint8_t count = 0;
    bool lastSeen = false;

    while (((len - offset) >= ADDR_LEN) && !lastSeen) {
        if (count == APRS_MAX_ADDRESSES)
            return 0; /* address field runs past what AX.25 allows */
        lastSeen = (data[offset + ADDR_LEN - 1] & ADDR_LAST) != 0;
        offset += ADDR_LEN;
        count++;
    }

    if (!lastSeen || (count < 2))
        return 0;

    return count;
}

bool aprsAddrFromFrame(const uint8_t *data, size_t len, uint8_t index,
                       struct aprsAddress *addr)
{
    if ((data == NULL) || (addr == NULL) || (len > APRS_PACLEN))
        return false;

    uint8_t count = countAddresses(data, len);
    if ((count == 0) || (index >= count))
        return false;

    memset(addr, 0, sizeof(*addr));
    parseAddress(&data[index * ADDR_LEN], addr);

    return true;
}

bool aprsPktFromFrame(const uint8_t *data, size_t len, struct aprsPacket *pkt)
{
    if ((data == NULL) || (pkt == NULL) || (len > APRS_PACLEN))
        return false;

    /* Validate the address field first, without writing anything, so a
     * malformed frame leaves the caller's packet untouched. */
    const uint8_t count = countAddresses(data, len);
    if (count == 0)
        return false;

    /* The frame also needs room for the control/PID pair and one info byte. */
    const size_t offset = (size_t)count * ADDR_LEN;
    if (offset + CTRL_PID_LEN >= len)
        return false;

    const size_t infoOffset = offset + CTRL_PID_LEN;
    const size_t infoLen = len - infoOffset;

    memset(pkt, 0, sizeof(*pkt));
    pkt->addressesLen = count;
    pkt->infoLen = (uint8_t)infoLen;

    for (uint8_t i = 0; i < count; i++)
        parseAddress(&data[i * ADDR_LEN], &pkt->addresses[i]);

    memcpy(pkt->info, &data[infoOffset], infoLen);
    pkt->info[infoLen] = '\0';
    pkt->type = classifyDti(pkt->info[0]);

    return true;
}

size_t aprsAddrToStr(const struct aprsAddress *addr, char *out, size_t len)
{
    if ((addr == NULL) || (out == NULL) || (len == 0))
        return 0;

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

size_t aprsPathToStr(const struct aprsPacket *pkt, char *out, size_t len)
{
    if ((pkt == NULL) || (out == NULL) || (len == 0))
        return 0;

    out[0] = '\0';
    size_t used = 0;

    for (uint8_t i = 2; i < pkt->addressesLen; i++) {
        if ((used + 1) >= len)
            break;

        if (used > 0)
            out[used++] = ',';

        used += aprsAddrToStr(&pkt->addresses[i], out + used, len - used);

        if (pkt->addresses[i].repeated && ((used + 1) < len))
            out[used++] = '*';

        out[used] = '\0';
    }

    return used;
}

/* ":ADDRESSEE:" — the addressee is always padded to nine characters, so the
 * separator that closes it sits at a fixed offset. */
#define MSG_ADDRESSEE_LEN APRS_MSG_ADDRESSEE_LEN
#define MSG_TEXT_OFFSET (1 + MSG_ADDRESSEE_LEN + 1)

bool aprsMsgUnwrap(const struct aprsPacket *pkt, char *addressee,
                   size_t addresseeLen, char *text, size_t textLen)
{
    if ((pkt == NULL) || (addressee == NULL) || (addresseeLen == 0)
        || (text == NULL) || (textLen == 0))
        return false;

    if (pkt->type != APRS_TYPE_MESSAGE)
        return false;

    /* The fixed-width addressee makes the layout checkable up front: a colon
     * in the right place is what separates a message from a bulletin-shaped
     * info field that merely starts with ':'. */
    if (pkt->infoLen < MSG_TEXT_OFFSET)
        return false;
    if (pkt->info[MSG_TEXT_OFFSET - 1] != ':')
        return false;

    size_t n = MSG_ADDRESSEE_LEN;
    while ((n > 0) && (pkt->info[n] == ' '))
        n--; /* strip the space padding */

    if (n == 0)
        return false; /* no addressee at all */
    if (n >= addresseeLen)
        n = addresseeLen - 1;

    memcpy(addressee, &pkt->info[1], n);
    addressee[n] = '\0';

    /* An acknowledgement-numbered message ends in "{seq"; the sequence is
     * transport bookkeeping, not something to show the user. */
    const char *body = &pkt->info[MSG_TEXT_OFFSET];
    size_t bodyLen = pkt->infoLen - MSG_TEXT_OFFSET;
    const char *brace = memchr(body, '{', bodyLen);
    if (brace != NULL)
        bodyLen = (size_t)(brace - body);

    if (bodyLen >= textLen)
        bodyLen = textLen - 1;

    memcpy(text, body, bodyLen);
    text[bodyLen] = '\0';

    return true;
}

/* ------------------------------------------------------------------------ *
 * Frame construction — the inverse of everything above.
 * ------------------------------------------------------------------------ */

/**
 * Shift one address into the seven bytes AX.25 wants, setting the flag bits
 * the caller asks for.
 */
static void writeAddress(uint8_t *dst, const struct aprsAddress *addr,
                         bool command, bool last)
{
    size_t i = 0;

    /* Callsigns shorter than six characters are padded with spaces, which
     * shift to 0x40 exactly where the parser expects to find them. */
    for (; (i < ADDR_LEN - 1) && (addr->addr[i] != '\0'); i++)
        dst[i] = (uint8_t)(addr->addr[i] << 1);
    for (; i < ADDR_LEN - 1; i++)
        dst[i] = (uint8_t)(' ' << 1);

    /* The two reserved bits are transmitted as ones, which is what every
     * other station sends and what the parser ignores. */
    dst[ADDR_LEN - 1] = 0x60;
    dst[ADDR_LEN - 1] |= (uint8_t)((addr->ssid & 0x0f) << ADDR_SSID_SHIFT);

    if (addr->repeated)
        dst[ADDR_LEN - 1] |= ADDR_REPEATED;
    if (command)
        dst[ADDR_LEN - 1] |= 0x80;
    if (last)
        dst[ADDR_LEN - 1] |= ADDR_LAST;
}

/**
 * Upper-case one ASCII letter; anything else is returned unchanged.
 */
static char upcase(char c)
{
    if ((c >= 'a') && (c <= 'z'))
        return (char)(c - 'a' + 'A');
    return c;
}

bool aprsAddrFromStr(const char *str, struct aprsAddress *addr)
{
    if ((str == NULL) || (addr == NULL))
        return false;

    struct aprsAddress parsed;
    memset(&parsed, 0, sizeof(parsed));

    size_t i = 0;
    for (; (str[i] != '\0') && (str[i] != '-'); i++) {
        if (i >= (ADDR_LEN - 1))
            return false; /* callsign longer than AX.25 can carry */

        char c = upcase(str[i]);
        /* Only the printable subset survives the shift-and-unshift round
         * trip, and a space would silently truncate the callsign. */
        if ((c <= ' ') || (c >= 0x7f))
            return false;

        parsed.addr[i] = c;
    }

    if (i == 0)
        return false; /* no callsign at all */

    parsed.addr[i] = '\0';

    if (str[i] == '-') {
        const char *digits = &str[i + 1];
        if (digits[0] == '\0')
            return false; /* a dash with nothing after it */

        unsigned ssid = 0;
        for (size_t j = 0; digits[j] != '\0'; j++) {
            if ((digits[j] < '0') || (digits[j] > '9'))
                return false;
            ssid = (ssid * 10u) + (unsigned)(digits[j] - '0');
            if (ssid > 15u)
                return false;
        }
        parsed.ssid = ssid & 0x0f;
    }

    *addr = parsed;
    return true;
}

size_t aprsFrameBuild(uint8_t *buf, size_t cap, const char *dest,
                      const char *src, const char *path, const char *info,
                      size_t infoLen)
{
    if ((buf == NULL) || (dest == NULL) || (src == NULL) || (info == NULL))
        return 0;
    if ((infoLen == 0) || (infoLen > APRS_PACLEN))
        return 0;

    /* Parse every address before writing anything, so a bad path leaves the
     * caller's frame untouched rather than half-built. */
    struct aprsAddress addresses[APRS_MAX_ADDRESSES];
    uint8_t count = 0;

    if (!aprsAddrFromStr(dest, &addresses[count++]))
        return 0;
    if (!aprsAddrFromStr(src, &addresses[count++]))
        return 0;

    if ((path != NULL) && (path[0] != '\0')) {
        const char *p = path;
        while (*p != '\0') {
            if (count >= APRS_MAX_ADDRESSES)
                return 0; /* more digipeaters than AX.25 allows */

            const char *comma = strchr(p, ',');
            size_t len = (comma != NULL) ? (size_t)(comma - p) : strlen(p);

            /* A path element carries the same "CALL-SSID" syntax as any
             * other address, optionally followed by the '*' that marks it as
             * already repeated — which aprsPathToStr() writes on the way
             * out, so accepting it here keeps the two symmetric. */
            char element[APRS_ADDR_STR_LEN];
            bool repeated = false;
            if ((len > 0) && (p[len - 1] == '*')) {
                repeated = true;
                len--;
            }
            if ((len == 0) || (len >= sizeof(element)))
                return 0;

            memcpy(element, p, len);
            element[len] = '\0';

            if (!aprsAddrFromStr(element, &addresses[count]))
                return 0;
            addresses[count].repeated = repeated ? 1 : 0;
            count++;

            if (comma == NULL)
                break;

            p = comma + 1;
            if (*p == '\0')
                return 0; /* trailing comma: an element that is not there */
        }
    }

    const size_t total = ((size_t)count * ADDR_LEN) + CTRL_PID_LEN + infoLen;
    if ((total > APRS_PACLEN) || (total > cap))
        return 0;

    for (uint8_t i = 0; i < count; i++) {
        /* The command bit belongs to the destination address; APRS sends UI
         * frames as commands, which is what every other station does. */
        writeAddress(&buf[(size_t)i * ADDR_LEN], &addresses[i], (i == 0),
                     (i == (count - 1)));
    }

    size_t offset = (size_t)count * ADDR_LEN;
    buf[offset++] = 0x03; /* UI frame, no acknowledgement */
    buf[offset++] = 0xf0; /* no layer 3 protocol          */

    memcpy(&buf[offset], info, infoLen);

    return total;
}

size_t aprsMsgFormat(char *out, size_t len, const char *addressee,
                     const char *text)
{
    if ((out == NULL) || (len == 0) || (addressee == NULL) || (text == NULL))
        return 0;

    size_t addrLen = strlen(addressee);
    size_t textLen = strlen(text);

    if ((addrLen == 0) || (addrLen > APRS_MSG_ADDRESSEE_LEN))
        return 0;
    if (textLen > APRS_MSG_TEXT_MAX)
        return 0;

    /* ':' + nine addressee characters + ':' + text + NUL. */
    const size_t total = MSG_TEXT_OFFSET + textLen;
    if (len < (total + 1))
        return 0;

    out[0] = ':';
    memcpy(&out[1], addressee, addrLen);
    for (size_t i = addrLen; i < APRS_MSG_ADDRESSEE_LEN; i++)
        out[1 + i] = ' ';
    out[MSG_TEXT_OFFSET - 1] = ':';

    memcpy(&out[MSG_TEXT_OFFSET], text, textLen);
    out[total] = '\0';

    return total;
}
