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

/* ":ADDRESSEE:" — the separator after the padded addressee is at a fixed
 * offset. */
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
