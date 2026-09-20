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
#define MSG_ADDRESSEE_LEN 9
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
