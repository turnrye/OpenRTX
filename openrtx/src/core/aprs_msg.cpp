/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * aprs_msg.cpp — APRS message source for the message inbox.
 *
 * A source is a codec, nothing more: the inbox owns the packet descriptors,
 * the storage and the lifecycle, and hands each received frame here to be
 * parsed and each outgoing entry here to be framed.  Both directions travel
 * as raw AX.25 bytes without the frame check sequence, which is what the
 * decoder produces, what the modulator consumes, and what a KISS host
 * supplies.
 *
 * Only addressed text messages for this station become inbox entries. The
 * channel is mostly positions, objects and status — measured at roughly
 * sixteen of every seventeen packets — and storing them would flush a
 * received message out of a bounded inbox within minutes. Everything heard
 * is still decoded and passed to the KISS emitter, so a host on the console
 * sees all of it.
 */

#include "core/aprs_msg.h"

#include "core/messages.h"
#include "core/packet_engine.h"
#include "core/state.h"
#include "protocols/APRS/packet.h"
#include "rtx/rtx.h"

#include <cerrno>
#include <cstring>

#ifdef PLATFORM_LINUX
#include <cstdio>
#endif

static_assert(APRS_PACLEN <= MSG_PKT_MAX_SIZE,
              "an AX.25 frame must fit in an inbox packet buffer");
static_assert(APRS_ADDR_STR_LEN <= MSG_ADDR_MAX_LEN,
              "a formatted APRS address must fit in a message address");

/**
 * \internal
 * This station's APRS address, as other stations must write it to reach us.
 */
static void myAddress(char *out, size_t len)
{
    strncpy(out, state.settings.callsign, len - 1);
    out[len - 1] = '\0';
}

static int processRx(const struct pktDesc *pkt)
{
    if ((pkt->buffer == nullptr) || (pkt->res <= 0))
        return -EINVAL;

    const auto *frame = static_cast<const uint8_t *>(pkt->buffer);
    const size_t len = (size_t)pkt->res;

    /* The host sees every frame the radio decodes, stored or not. */
    packetEngine_send(frame, len);

    /* Static: a parsed packet is a couple of hundred bytes and this runs on
     * a thread with a small stack; it is never re-entered. */
    static struct aprsPacket parsed;
    static char addressee[APRS_ADDR_STR_LEN];
    static char text[APRS_PACLEN];

    if (!aprsPktFromFrame(frame, len, &parsed))
        return -EPROTO;

    char sender[APRS_ADDR_STR_LEN];
    aprsAddrToStr(&parsed.addresses[1], sender, sizeof(sender));

    if (parsed.type != APRS_TYPE_MESSAGE) {
#ifdef PLATFORM_LINUX
        fprintf(stderr, "APRS_DECODED from '%s' type %u: '%s'\n", sender,
                (unsigned)parsed.type, parsed.info);
#endif
        return 0; /* decoded, deliberately not stored */
    }

    if (!aprsMsgUnwrap(&parsed, addressee, sizeof(addressee), text,
                       sizeof(text)))
        return -EPROTO;

    char me[APRS_ADDR_STR_LEN];
    myAddress(me, sizeof(me));
    if (strcmp(addressee, me) != 0) {
#ifdef PLATFORM_LINUX
        fprintf(stderr, "APRS_DECODED from '%s' type %u to '%s': '%s'\n",
                sender, (unsigned)parsed.type, addressee, text);
#endif
        return 0; /* somebody else's message */
    }

    struct message msg = {};
    msg.body = text;
    msg.bodyLen = (uint16_t)strlen(text);
    msg.type = (uint16_t)parsed.type;
    msg.mode = OPMODE_APRS;
    msg.direction = MSG_DIR_RX;
    msg.status = MSG_STATUS_RECEIVED;
    msg.unread = 1;
    strncpy(msg.sender, sender, sizeof(msg.sender) - 1);
    strncpy(msg.recipient, addressee, sizeof(msg.recipient) - 1);

    int32_t seq = messages_store(&msg);
    if (seq < 0)
        return seq;

#ifdef PLATFORM_LINUX
    /* A marker the e2e tests grep for; only ever useful under the
     * emulator, and fprintf is not something to run on an embedded stack. */
    fprintf(stderr, "APRS_RECEIVED from '%s' type %u: '%s'\n", msg.sender,
            (unsigned)msg.type, text);
#endif

    return 0;
}

static int formatTx(const struct message *msg, struct pktDesc *pkt)
{
    if ((pkt->buffer == nullptr) || (pkt->size < APRS_PACLEN))
        return -EINVAL;

    if ((msg->bodyLen == 0) || (msg->recipient[0] == '\0'))
        return -EINVAL;

    /* An APRS message is capped at 67 characters of text by the
     * specification, far short of what the compose field can hold, so an
     * over-long message is refused rather than silently truncated into
     * something the user did not write. */
    if (msg->bodyLen > APRS_MSG_TEXT_MAX)
        return -EMSGSIZE;

    /* Refuse to transmit under the factory callsign. APRS frames carry the
     * sender's callsign to every station that hears them and to the
     * internet behind them, so transmitting as N0CALL is both unidentified
     * operation and a small act of pollution. Kenwood's APRS radios take the
     * same position: they ship as NOCALL and block APRS transmission until
     * it is changed. */
    if ((msg->sender[0] == '\0') || (strcmp(msg->sender, "N0CALL") == 0))
        return -EINVAL;

    static char info[APRS_MSG_ADDRESSEE_LEN + APRS_MSG_TEXT_MAX + 3];
    static char text[APRS_MSG_TEXT_MAX + 1];

    memcpy(text, msg->body, msg->bodyLen);
    text[msg->bodyLen] = '\0';

    const size_t infoLen = aprsMsgFormat(info, sizeof(info), msg->recipient,
                                         text);
    if (infoLen == 0)
        return -EINVAL;

    const size_t frameLen = aprsFrameBuild(static_cast<uint8_t *>(pkt->buffer),
                                           pkt->size, APRS_TOCALL, msg->sender,
                                           APRS_DEFAULT_PATH, info, infoLen);
    if (frameLen == 0)
        return -EINVAL;

    pkt->size = frameLen;
    return 0;
}

const struct messageOps aprs_msg_ops = {
    processRx,
    formatTx,
    "APRS",
    OPMODE_APRS,
};
