/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * APRS message source for the message inbox. The inbox owns the packet
 * descriptors, the storage and the lifecycle; this source parses each
 * received frame and frames each outgoing entry, as raw AX.25 without the
 * frame check sequence.
 *
 * Only addressed text messages for this station become inbox entries: the
 * rest of the channel, positions, objects and status, goes to the station
 * table and the traffic log instead, so it cannot flush messages out of a
 * bounded inbox.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include "core/aprs_msg.h"
#include "core/aprs_traffic.h"
#include "core/messages.h"
#include "core/packet_engine.h"
#include "core/state.h"
#include "core/stations.h"
#include "core/utils.h"
#include "interfaces/delays.h"
#include "protocols/APRS/packet.h"
#include "protocols/M17/AprsPacket.h"
#include "protocols/M17/m17.h"
#include "rtx/rtx.h"

#ifdef PLATFORM_LINUX
#include <stdio.h>
#endif

#if APRS_PACLEN > MSG_PKT_MAX_SIZE
#error "An AX.25 frame must fit in an inbox packet buffer"
#endif

#if APRS_ADDR_STR_LEN > MSG_ADDR_MAX_LEN
#error "A formatted APRS address must fit in a message address"
#endif

#ifndef CONFIG_APRS_MSG_HEARD
#define CONFIG_APRS_MSG_HEARD 8
#endif

/* Times in getTick() milliseconds. APRS 1.2 wants every copy of a message
 * acknowledged, but repeated acks 30 s apart: digipeaters drop duplicates
 * inside that window. */
#define ACK_GAP 30000LL
/* An ack still unsent by then answers a copy the sender has moved past. */
#define ACK_STALE 60000LL
/* Copies of a numbered message keep coming while the sender retries; an
 * unnumbered one is only repeated by digipeaters. */
#define DUPE_NUMBERED (30LL * 60000LL)
#define DUPE_PLAIN 30000LL

/** A message recently received for us. */
struct heardMsg {
    char from[APRS_ADDR_STR_LEN];
    struct aprsMsgId id; /**< As in the latest copy. */
    uint32_t textHash;
    long long heard;     /**< Latest copy. */
    long long acked;     /**< Latest ack sent. */
    uint8_t mode;        /**< Opmode the latest copy came in, for the ack. */
    bool ackDue;
    bool used;
};

static struct heardMsg heard[CONFIG_APRS_MSG_HEARD];

#ifndef CONFIG_APRS_MSG_AWAITING
#define CONFIG_APRS_MSG_AWAITING 4
#endif

/* Wait after each transmission of an unacknowledged message, never shorter
 * than the 30 s duplicate window of digipeaters and APRS-IS; 35 s first, as
 * a copy gated 30 s later can still land inside APRS-IS's window. */
static const long long RETRY_GAP[] = { 35000, 60000, 120000, 240000 };
#define MAX_SENDS ARRAY_SIZE(RETRY_GAP)

/** A numbered message of ours, waiting for its acknowledgement. */
struct awaitingAck {
    uint32_t sequence; /**< Inbox entry. */
    char to[APRS_ADDR_STR_LEN];
    char number[3];
    uint8_t sends;  /**< Zero until the first transmission completes. */
    uint8_t mode;   /**< Opmode it travels in, for its retries. */
    long long next; /**< When the next retry is due. */
    bool used;
};

static struct awaitingAck awaiting[CONFIG_APRS_MSG_AWAITING];
static uint16_t lastNumber;

/**
 * \internal
 * This station's APRS address, as other stations must write it to reach us.
 */
static void myAddress(char *out, size_t len)
{
    strncpy(out, state.settings.aprs_callsign, len - 1);
    out[len - 1] = '\0';
}

/**
 * \internal
 * Whether @p addressee names this station: compared as addresses, so
 * "n0call-0" still reaches N0CALL.
 */
static bool addressedToMe(const char *addressee, const char *me)
{
    struct aprsAddress a, b;

    if ((aprsAddr_fromStr(addressee, &a) != 0)
        || (aprsAddr_fromStr(me, &b) != 0))
        return strcmp(addressee, me) == 0;

    return (strcmp(a.addr, b.addr) == 0) && (a.ssid == b.ssid);
}

/* The emergency alert: who raised it, when, and whether it is still unseen. */
static char emergencyCall[APRS_ADDR_STR_LEN];
static uint32_t emergencyAt;
static bool emergencyPending;
static char emergencyAckCall[APRS_ADDR_STR_LEN];
static uint32_t emergencyAckAt;
#define EMERGENCY_ACK_HOLD 600u /* seconds */

/**
 * \internal
 * A station raised an emergency: alert, unless the operator acknowledged
 * this station's emergency recently.
 */
static void raiseEmergency(const char *call, uint32_t now)
{
    if ((strcmp(call, emergencyAckCall) == 0)
        && ((now - emergencyAckAt) < EMERGENCY_ACK_HOLD))
        return;

    strncpy(emergencyCall, call, sizeof(emergencyCall) - 1);
    emergencyCall[sizeof(emergencyCall) - 1] = '\0';
    emergencyAt = now;
    emergencyPending = true;
}

/**
 * \internal
 * The digipeater path for what we send: the one configured, else the
 * conventional default.
 */
static const char *txPath(void)
{
    const char *path = state.settings.aprs_path;
    return (path[0] != '\0') ? path : APRS_DEFAULT_PATH;
}

/**
 * \internal
 * Whether a callsign identifies anyone. N0CALL is the factory default, and
 * transmitting under it is unidentified operation.
 */
static bool identified(const char *call)
{
    return (call[0] != '\0') && (strcmp(call, "N0CALL") != 0);
}

/** \internal FNV-1a, enough to tell two message texts apart. */
static uint32_t textHash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s != '\0'; s++)
        h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

/**
 * \internal
 * Record a message received for us. The reply-ack is not part of a message's
 * identity: it may change between copies.
 *
 * @return true if this is the first copy, false for a duplicate.
 */
static bool rememberHeard(const char *from, const struct aprsMsgId *id,
                          const char *text, long long now, uint8_t mode)
{
    const uint32_t hash = textHash(text);
    const long long window = (id->number[0] != '\0') ? DUPE_NUMBERED :
                                                       DUPE_PLAIN;
    struct heardMsg *h = NULL;
    struct heardMsg *oldest = &heard[0];

    for (size_t i = 0; i < ARRAY_SIZE(heard); i++) {
        struct heardMsg *c = &heard[i];
        if (c->used && (strcmp(c->from, from) == 0)
            && (strcmp(c->id.number, id->number) == 0) && (c->textHash == hash)
            && ((now - c->heard) < window)) {
            h = c;
            break;
        }

        if (!c->used || (oldest->used && (c->heard < oldest->heard)))
            oldest = c;
    }

    const bool first = (h == NULL);
    if (first) {
        h = oldest;
        memset(h, 0, sizeof(*h));
        strncpy(h->from, from, sizeof(h->from) - 1);
        h->textHash = hash;
        h->acked = now - ACK_GAP;
        h->used = true;
    }

    h->id = *id;
    h->heard = now;
    h->mode = mode;
    if ((id->number[0] != '\0') && ((now - h->acked) >= ACK_GAP))
        h->ackDue = true;

    return first;
}

/**
 * \internal
 * The reply-ack owed to @p to: the number of its latest message, if it sent
 * one in the reply-ack form. Only a short number keeps "{MM}AA" within the
 * five characters older software expects.
 *
 * @return the number, or NULL if none is owed.
 */
static const char *replyAckFor(const char *to, long long now)
{
    const struct heardMsg *latest = NULL;

    for (size_t i = 0; i < ARRAY_SIZE(heard); i++) {
        const struct heardMsg *h = &heard[i];
        if (h->used && (strcasecmp(h->from, to) == 0) && h->id.replyAckCapable
            && (h->id.number[0] != '\0') && (strlen(h->id.number) <= 2)
            && ((now - h->heard) < DUPE_NUMBERED)
            && ((latest == NULL) || (h->heard > latest->heard)))
            latest = h;
    }

    return (latest != NULL) ? latest->id.number : NULL;
}

/**
 * \internal
 * Lay out an AX.25 frame as APRS in an M17 packet, from our M17 callsign to
 * everyone.
 *
 * @return packet length, or 0 without an M17 callsign or room.
 */
static size_t m17Wrap(const uint8_t *frame, size_t len, uint8_t *out,
                      size_t cap)
{
    const size_t src = offsetof(struct m17Packet, src);
    const size_t dst = offsetof(struct m17Packet, dst);
    const size_t payload = offsetof(struct m17Packet, payload);
    const size_t call = sizeof(((struct m17Packet *)0)->src);

    if (!identified(state.settings.callsign) || (cap <= payload))
        return 0;

    memset(out, 0, payload);
    strncpy((char *)&out[src], state.settings.callsign, call - 1);
    strncpy((char *)&out[dst], "ALL", call - 1);

    size_t n = m17_aprsFormat(frame, len, &out[payload], cap - payload);
    return (n == 0) ? 0 : payload + n;
}

/**
 * \internal
 * Transmit an AX.25 frame as APRS in M17 packet mode. There are no
 * digipeaters on M17: the frame carries no path.
 */
static int sendOverM17(const uint8_t *frame, size_t len)
{
    static uint8_t buf[offsetof(struct m17Packet, payload) + APRS_PACLEN + 64];

    size_t n = m17Wrap(frame, len, buf, sizeof(buf));
    if (n == 0)
        return -EINVAL;

    return messages_sendPacket(OPMODE_M17, buf, n);
}

/**
 * \internal
 * Transmit one acknowledgement that is due, if the transmitter is free.
 */
static void sendAcks(long long now, uint8_t mode)
{
    char me[APRS_ADDR_STR_LEN];
    myAddress(me, sizeof(me));
    if (!identified(me))
        return;

    for (size_t i = 0; i < ARRAY_SIZE(heard); i++) {
        struct heardMsg *h = &heard[i];
        /* An ack goes back the way its message came. */
        if (!h->used || !h->ackDue || (h->mode != mode))
            continue;

        if ((now - h->heard) > ACK_STALE) {
            h->ackDue = false;
            continue;
        }

        static char info[APRS_MSG_INFO_MAX + 1];
        static uint8_t frame[APRS_PACLEN];
        ssize_t len = (ssize_t)aprsAckFormat(info, sizeof(info), h->from,
                                             APRS_MSG_ACK, &h->id);
        if (len > 0)
            len = aprsFrame_build(frame, sizeof(frame), APRS_TOCALL, me,
                                  (mode == OPMODE_M17) ? NULL : txPath(), info,
                                  (size_t)len);

        int ret = -EINVAL;
        if ((len > 0) && (mode == OPMODE_M17))
            ret = sendOverM17(frame, (size_t)len);
        else if (len > 0)
            ret = messages_sendPacket(OPMODE_APRS, frame, (size_t)len);
        if (ret == -EBUSY)
            return;

        h->ackDue = false;
        if (ret == 0)
            h->acked = now;
        return;
    }
}

/**
 * \internal
 * The record for an outgoing message, created with a fresh two-character
 * number on its first transmission. With every record taken, the oldest
 * message is given up on.
 */
static struct awaitingAck *awaitingFor(const struct message *msg)
{
    static const char digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    struct awaitingAck *slot = NULL;

    for (size_t i = 0; i < ARRAY_SIZE(awaiting); i++) {
        struct awaitingAck *a = &awaiting[i];

        if (a->used && (a->sequence == msg->sequence))
            return a;
        if ((slot == NULL) && !a->used)
            slot = a;
    }

    if (slot == NULL) {
        slot = &awaiting[0];
        for (size_t i = 0; i < ARRAY_SIZE(awaiting); i++) {
            if (awaiting[i].sequence < slot->sequence)
                slot = &awaiting[i];
        }
        messages_setStatus(slot->sequence, MSG_STATUS_UNACKED);
    }

    lastNumber = (uint16_t)((lastNumber % ((36 * 36) - 1)) + 1);
    memset(slot, 0, sizeof(*slot));
    slot->sequence = msg->sequence;
    slot->mode = messages_travelMode(msg);
    strncpy(slot->to, msg->recipient, sizeof(slot->to) - 1);
    slot->number[0] = digits[lastNumber / 36];
    slot->number[1] = digits[lastNumber % 36];
    slot->used = true;

    return slot;
}

/**
 * \internal
 * Settle our message @p number to @p from with an acknowledgement or a
 * rejection.
 */
static void settleAwaiting(const char *from, const struct aprsMsgId *id)
{
    for (size_t i = 0; i < ARRAY_SIZE(awaiting); i++) {
        struct awaitingAck *a = &awaiting[i];
        if (a->used && (strcasecmp(a->to, from) == 0)
            && (strcasecmp(a->number, id->number) == 0)) {
            messages_setStatus(a->sequence, (id->kind == APRS_MSG_REJ) ?
                                                MSG_STATUS_REJECTED :
                                                MSG_STATUS_DELIVERED);
            a->used = false;
            return;
        }
    }
}

/**
 * \internal
 * Retry messages still waiting for an acknowledgement, and give up on those
 * that ran out of retries.
 */
static void retryAwaiting(long long now, uint8_t mode)
{
    for (size_t i = 0; i < ARRAY_SIZE(awaiting); i++) {
        struct awaitingAck *a = &awaiting[i];
        /* A retry goes the way the message went. */
        if (!a->used || (a->mode != mode))
            continue;

        size_t idx = messages_findBySequence(a->sequence);
        const struct message *m = messages_get(idx);
        if (m == NULL) {
            a->used = false;
            continue;
        }

        if (a->sends == 0) {
            if (m->status == MSG_STATUS_SENT) {
                a->sends = 1;
                a->next = now + RETRY_GAP[0];
            } else if (m->status != MSG_STATUS_SENDING) {
                a->used = false;
            }
            continue;
        }

        if (m->status != MSG_STATUS_SENT) {
            a->used = false;
            continue;
        }

        if (now < a->next)
            continue;

        if (a->sends >= MAX_SENDS) {
            messages_setStatus(a->sequence, MSG_STATUS_UNACKED);
            a->used = false;
            continue;
        }

        int ret = messages_resend(a->sequence);
        if (ret == 0) {
            a->next = now + RETRY_GAP[a->sends];
            a->sends++;
        } else if (ret != -EBUSY) {
            messages_setStatus(a->sequence, MSG_STATUS_FAILED);
            a->used = false;
        }
    }
}

/**
 * \internal
 * Put what a packet says about its sender, or about the object it places,
 * into the station table.
 */
static void noteStation(const struct aprsPacket *pkt, const char *sender,
                        uint32_t now, uint8_t carrier)
{
    static struct aprsPosition pos;
    static struct aprsFreq freq;
    bool hasPos = aprsPosParse(pkt, &pos);
    bool hasFreq = aprsFreqParse(pkt, &freq);

    /* The sender was heard, whatever it sent. */
    struct station *s = stations_update(sender, carrier, now);
    if (s == NULL)
        return;

    if (aprsEmergency(pkt))
        s->valid |= STATION_EMERGENCY;
    else
        s->valid &= (uint8_t)~STATION_EMERGENCY;

    if ((pkt->type == APRS_TYPE_WEATHER)
        || (hasPos && (pos.name[0] == '\0') && (pos.symbolCode == '_')))
        s->kind = STATION_KIND_WEATHER;

    /* Kept until another replaces it: a tracker may send it only in its
     * status. */
    if (!hasPos) {
        if (hasFreq) {
            s->freq = freq;
            s->valid |= STATION_HAS_FREQ;
        }
        return;
    }

    /* An object or item is a record of its own, placed by the sender. */
    if (pos.name[0] != '\0') {
        if (pos.killed) {
            stations_remove(pos.name);
            return;
        }
        s = stations_update(pos.name, carrier, now);
        if (s == NULL)
            return;
        s->kind = (pkt->info[0] == ';') ? STATION_KIND_OBJECT :
                                          STATION_KIND_ITEM;
    }

    s->latitude = pos.latitude;
    s->longitude = pos.longitude;
    s->symTable = pos.symbolTable;
    s->symCode = pos.symbolCode;
    s->valid = (uint8_t)((s->valid & (STATION_EMERGENCY | STATION_HAS_FREQ))
                         | STATION_HAS_POSITION | STATION_HAS_SYMBOL);
    if (hasFreq) {
        s->freq = freq;
        s->valid |= STATION_HAS_FREQ;
    }
    if (pos.ambiguous)
        s->valid |= STATION_POSITION_AMBIGUOUS;
    if (pos.hasCourseSpeed) {
        s->course = (int16_t)pos.course;
        s->speed = pos.speed;
        s->valid |= STATION_HAS_VELOCITY;
    }
    if (pos.hasAltitude) {
        s->altitude = pos.altitude;
        s->valid |= STATION_HAS_ALTITUDE;
    }
}

/**
 * \internal
 * Handle an AX.25 frame heard in @p mode: OPMODE_APRS on the AFSK channel,
 * OPMODE_M17 when it came in M17 packet mode.
 */
static int processFrame(const uint8_t *frame, size_t len, uint8_t mode)
{
    // A host on the console sees every frame heard, stored or not
    packetEngine_send(frame, len);

    /* Static: a parsed packet is a few hundred bytes and this runs on a
     * thread with a small stack; it is never re-entered. */
    static struct aprsPacket heardPkt;
    static struct aprsPacket inner;
    static char addressee[APRS_ADDR_STR_LEN];
    static char text[APRS_PACLEN];

    int ret = aprsPkt_fromFrame(frame, len, &heardPkt);
    if (ret != 0)
        return ret;

    char sender[APRS_ADDR_STR_LEN];
    aprsAddr_toStr(&heardPkt.addresses[1], sender, sizeof(sender));

    const uint32_t now = (uint32_t)(getTick() / 1000);
    const uint8_t carrier = (mode == OPMODE_M17) ? STATION_CARRIER_M17 :
                                                   STATION_CARRIER_APRS;
    noteStation(&heardPkt, sender, now, carrier);

    /* Third-party traffic: the log keeps the packet as heard; everything
     * else takes what it carries as sent by its source, whose name AX.25
     * may not hold. */
    const struct aprsPacket *pkt = &heardPkt;
    uint8_t flags = (mode == OPMODE_M17) ? APRS_TRAFFIC_M17 : 0;
    if (aprsThirdPartyUnwrap(&heardPkt, &inner, sender, sizeof(sender))) {
        pkt = &inner;
        flags |= APRS_TRAFFIC_THIRD_PARTY;
        noteStation(&inner, sender, now, carrier);
    }

    char me[APRS_ADDR_STR_LEN];
    myAddress(me, sizeof(me));
    const enum aprsKind kind = aprsPacketKind(pkt, me);
    if (kind == APRS_KIND_EMERGENCY) {
        flags |= APRS_TRAFFIC_EMERGENCY;
        raiseEmergency(sender, now);
    } else if (kind == APRS_KIND_GROUP) {
        flags |= APRS_TRAFFIC_GROUP;
    }

    if (pkt->type != APRS_TYPE_MESSAGE) {
#ifdef PLATFORM_LINUX
        fprintf(stderr, "APRS_DECODED from '%s' type %u: '%.*s'\n", sender,
                (unsigned)pkt->type, (int)pkt->infoLen, pkt->info);
#endif
        aprsTraffic_record(&heardPkt, flags, now);
        return 0; /* decoded, deliberately not stored */
    }

    if (aprsMsg_unwrap(pkt, addressee, sizeof(addressee), text, sizeof(text))
        != 0)
        return -EPROTO;

    /* An emergency message to us is still a message to us. */
    const bool forMe = addressedToMe(addressee, me);
    if (forMe)
        flags |= APRS_TRAFFIC_FOR_ME;
    aprsTraffic_record(&heardPkt, flags, now);
    if (!forMe) {
#ifdef PLATFORM_LINUX
        fprintf(stderr, "APRS_DECODED from '%s' type %u to '%s': '%s'\n",
                sender, (unsigned)pkt->type, addressee, text);
#endif
        return 0; /* somebody else's message */
    }

    static struct aprsMsgId id;
    if (!aprsMsgParseId(pkt, &id))
        return -EPROTO;

    if (id.kind != APRS_MSG_TEXT) {
        settleAwaiting(sender, &id);
        return 0; /* an acknowledgement, not a message */
    }

    /* "{MM}AA" also acknowledges our message AA. */
    if (id.replyAck[0] != '\0') {
        struct aprsMsgId ack;
        memset(&ack, 0, sizeof(ack));
        ack.kind = APRS_MSG_ACK;
        memcpy(ack.number, id.replyAck, sizeof(ack.number));
        settleAwaiting(sender, &ack);
    }

    aprsSanitize(text);
    if (!rememberHeard(sender, &id, text, getTick(), mode))
        return 0; /* a copy of one already stored */

    struct message msg;
    memset(&msg, 0, sizeof(msg));
    msg.body = text;
    msg.bodyLen = (uint16_t)strlen(text);
    msg.type = (uint16_t)pkt->type;
    msg.mode = OPMODE_APRS;
    msg.carrier = (mode == OPMODE_M17) ? OPMODE_M17 : 0;
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

static int processRx(const struct pktDesc *pkt)
{
    if (pkt->res <= 0)
        return -EINVAL;

    return processFrame((const uint8_t *)pkt->buffer, (size_t)pkt->res,
                        OPMODE_APRS);
}

/** \internal APRS carried in M17 packet mode. */
static int processGuest(uint8_t mode, const struct pktDesc *pkt)
{
    static uint8_t frame[APRS_PACLEN];

    if ((mode != OPMODE_M17) || (pkt->res <= 0))
        return -EPROTO;

    const struct m17Packet *packet = (const struct m17Packet *)pkt->buffer;
    size_t n = m17_aprsFrame(packet->payload, (size_t)pkt->res, frame,
                             sizeof(frame));
    if (n == 0)
        return -EPROTO;

    return processFrame(frame, n, OPMODE_M17);
}

static int formatTx(const struct message *msg, struct pktDesc *pkt)
{
    if (pkt->size < APRS_PACLEN)
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
    if (!identified(msg->sender))
        return -EINVAL;

    static char info[APRS_MSG_INFO_MAX + 1];
    static char text[APRS_MSG_TEXT_MAX + 1];

    memcpy(text, msg->body, msg->bodyLen);
    text[msg->bodyLen] = '\0';

    /* Numbered, so the recipient acknowledges it; a retry keeps its number.
     * The reply-ack is picked afresh for every transmission. */
    struct aprsMsgId id;
    memset(&id, 0, sizeof(id));
    memcpy(id.number, awaitingFor(msg)->number, 3);
    id.replyAckCapable = true;
    const char *aa = replyAckFor(msg->recipient, getTick());
    if (aa != NULL)
        strncpy(id.replyAck, aa, sizeof(id.replyAck) - 1);

    const size_t infoLen = aprsMsgFormatId(info, sizeof(info), msg->recipient,
                                           text, &id);
    if (infoLen == 0)
        return -EINVAL;

    /* In M17 packet mode there are no digipeaters: no path. */
    const bool overM17 = (msg->carrier == OPMODE_M17);
    static uint8_t frame[APRS_PACLEN];
    const ssize_t built =
        aprsFrame_build(frame, sizeof(frame), APRS_TOCALL, msg->sender,
                        overM17 ? NULL : txPath(), info, infoLen);
    if (built < 0)
        return (int)built;

    const size_t frameLen = (size_t)built;

    /* Sent frames show in the traffic log too. */
    static struct aprsPacket sent;
    if (aprsPkt_fromFrame(frame, frameLen, &sent) == 0)
        aprsTraffic_record(&sent,
                           APRS_TRAFFIC_TX | (overM17 ? APRS_TRAFFIC_M17 : 0),
                           (uint32_t)(getTick() / 1000));

    uint8_t *out = (uint8_t *)pkt->buffer;
    if (overM17) {
        size_t n = m17Wrap(frame, frameLen, out, pkt->size);
        if (n == 0)
            return -EINVAL;
        pkt->size = n;
        return 0;
    }

    memcpy(out, frame, frameLen);
    pkt->size = frameLen;
    return 0;
}

static void task(void)
{
    const long long now = getTick();

    sendAcks(now, OPMODE_APRS);
    retryAwaiting(now, OPMODE_APRS);
}

/** \internal Acks and retries for APRS carried in M17 packet mode. */
static void guestTask(uint8_t mode)
{
    if (mode == OPMODE_M17) {
        const long long now = getTick();

        sendAcks(now, OPMODE_M17);
        retryAwaiting(now, OPMODE_M17);
    }
}

void aprs_msg_reset(void)
{
    memset(heard, 0, sizeof(heard));
    memset(awaiting, 0, sizeof(awaiting));
    lastNumber = 0;
    emergencyPending = false;
    emergencyCall[0] = '\0';
    emergencyAckCall[0] = '\0';
    emergencyAckAt = 0;
}

bool aprs_msg_emergency(char *call, size_t len, uint32_t *since)
{
    if (!emergencyPending)
        return false;

    if ((call != NULL) && (len > 0)) {
        strncpy(call, emergencyCall, len - 1);
        call[len - 1] = '\0';
    }
    if (since != NULL)
        *since = emergencyAt;

    return true;
}

void aprs_msg_emergencyAck(void)
{
    if (!emergencyPending)
        return;

    emergencyPending = false;
    strncpy(emergencyAckCall, emergencyCall, sizeof(emergencyAckCall));
    emergencyAckAt = (uint32_t)(getTick() / 1000);
}

/* APRS has no broadcast address for messages. */
const struct messageOps aprs_msg_ops = {
    .processRx = processRx,
    .formatTx = formatTx,
    .name = "APRS",
    .mode = OPMODE_APRS,
    .broadcastAddr = NULL,
    .task = task,
    .maxBodyLen = APRS_MSG_TEXT_MAX,
    .processGuest = processGuest,
    .guestTask = guestTask,
};
