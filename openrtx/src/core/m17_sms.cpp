/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * m17_sms.cpp — M17 SMS message source for the generic message inbox.
 *
 * Thread model:
 *   RTX thread: m17_sms_task_rtx() — owns tx_buf/tx_desc/rx_buf/rx_desc.
 *     * Drains pkt_tx_request_t from packet_io; formats and submits pktDesc.
 *     * On TX completion posts a pkt_rx_event_t (status = SENT or FAILED).
 *     * On RX completion parses payload; posts pkt_rx_event_t (status = RECEIVED).
 *
 *   UI thread: sms_tick() (called via vtable->tick from messages_tick())
 *     and sms_send() (called via vtable->send from messages_send()).
 *     Both run on the UI thread and are the sole owners of entries[]/body_pool.
 *
 * packet_io provides the single mutex protecting the queue slots; it is held
 * only during memcpy, keeping contention negligible.
 */

#include "hwconfig.h"

#ifdef CONFIG_M17_SMS

#include "core/m17_sms.h"
#include "core/packet_io.h"
#include "interfaces/platform.h"
#include "protocols/M17/SmsPacket.hpp"
#include "rtx/rtx.h"

#include <cerrno>
#ifdef PLATFORM_LINUX
#include <cstdio>
#endif
#include <cstdint>
#include <cstring>

/* M17 packet data buffer size — matches PacketFramer/Deframer max payload. */
#define SMS_PKT_BUF 825

static_assert(M17_SMS_MAX_MESSAGES > 0, "M17_SMS_MAX_MESSAGES must be > 0");
static_assert(M17_SMS_POOL_BYTES >= PKT_BODY_MAX_LEN,
              "M17_SMS_POOL_BYTES must be >= PKT_BODY_MAX_LEN (822)");
static_assert(M17_SMS_POOL_BYTES <= 65535,
              "M17_SMS_POOL_BYTES exceeds uint16_t range; "
              "reduce the value or widen pool_head / pool offsets");

/* ===================================================================
 * UI-thread storage (entries[], body_pool)
 * Owned exclusively by the UI thread; no lock needed.
 * =================================================================== */

/**
 * One stored SMS.  The message_header_t MUST be the first member so that
 * a vtable get() return value can be reinterpret_cast'd back to SmsEntry.
 */
struct SmsEntry {
    message_header_t hdr; /**< Registry-visible header. Must be first. */
    uint16_t pool_offset; /**< Byte offset of body text in body_pool. */
    uint16_t pool_len;    /**< Length of body text (not counting NUL). */
    bool active;          /**< Slot is in use. */
    uint32_t tag;         /**< Echoed from pkt_tx_request_t for completion
                               matching; 0 for RX entries. */
};

static SmsEntry entries[M17_SMS_MAX_MESSAGES];

/**
 * Variable-length body text pool.  Bodies are stored as NUL-terminated
 * strings in a ring buffer.  pool_head points to the next byte to write.
 * When a new body would wrap or overwrite existing data, any active entry
 * whose pool region overlaps the write range is evicted first.
 */
static char body_pool[M17_SMS_POOL_BYTES];
static uint16_t pool_head = 0;

/* ===================================================================
 * RTX-thread storage (tx_buf, tx_desc, rx_buf, rx_desc)
 * Owned exclusively by the RTX thread; no lock needed.
 * =================================================================== */

static uint8_t tx_buf[SMS_PKT_BUF];
static struct pktDesc tx_desc;
static uint32_t tx_inflight_tag = 0; /* tag of the in-flight TX request */

static uint8_t rx_buf[SMS_PKT_BUF];
static struct pktDesc rx_desc;

/* Working buffers for m17_sms_task_rtx().
 * Declared at file scope because the RTX thread has a 512-byte stack on
 * embedded targets.  pkt_tx_request_t (~843 B) and pkt_rx_event_t (~847 B)
 * cannot be allocated as local variables in that context. */
static pkt_tx_request_t rtx_req;
static pkt_rx_event_t rtx_evt;    /* TX completion event retry buffer */
static pkt_rx_event_t rtx_rx_evt; /* RX received event retry buffer */
static bool rtx_tx_evt_ready = false;
static bool rtx_rx_evt_ready = false;
static char rtx_rx_text[PKT_BODY_MAX_LEN];

/* ===================================================================
 * UI-thread internal helpers
 * =================================================================== */

/**
 * Allocate len+1 bytes in body_pool for a new body.
 *
 * Advances pool_head by len+1 (wrapping at M17_SMS_POOL_BYTES).  Any active
 * entry whose pool region overlaps the write range is evicted — those entries
 * are always older because pool_head only moves forward.
 *
 * @param len: length of the body text (not counting NUL).
 * @return byte offset in body_pool at which to write.
 */
static uint16_t pool_alloc(uint16_t len)
{
    uint16_t need = len + 1; /* +1 for NUL */

    /* Wrap to start if the write would run off the end. */
    if ((uint32_t)pool_head + need > M17_SMS_POOL_BYTES)
        pool_head = 0;

    uint16_t ws = pool_head;
    uint16_t we = pool_head + need;

    /* Evict any active entry whose body overlaps [ws, we). */
    for (size_t i = 0; i < M17_SMS_MAX_MESSAGES; i++) {
        if (!entries[i].active)
            continue;
        uint16_t es = entries[i].pool_offset;
        uint16_t ee = entries[i].pool_offset + entries[i].pool_len + 1;
        if (es < we && ee > ws)
            entries[i].active = false;
    }

    pool_head = we;
    return ws;
}

/**
 * Find a free (inactive) slot.
 * If the pool is full, evict the entry with the oldest timestamp.
 * @return index into entries[].
 */
static size_t alloc_slot(void)
{
    /* First pass: look for a free slot. */
    for (size_t i = 0; i < M17_SMS_MAX_MESSAGES; i++) {
        if (!entries[i].active)
            return i;
    }

    /* Pool full — evict the chronologically oldest entry. */
    size_t oldest = 0;
    for (size_t i = 1; i < M17_SMS_MAX_MESSAGES; i++) {
        if (datetime_cmp(&entries[i].hdr.timestamp,
                         &entries[oldest].hdr.timestamp)
            < 0) {
            oldest = i;
        }
    }

    entries[oldest].active = false;
    return oldest;
}

/* ===================================================================
 * Vtable callbacks — all called from the UI thread
 * =================================================================== */

static size_t sms_count(void *ctx)
{
    (void)ctx;
    size_t n = 0;
    for (size_t i = 0; i < M17_SMS_MAX_MESSAGES; i++)
        if (entries[i].active)
            n++;
    return n;
}

static message_header_t *sms_get(void *ctx, size_t idx)
{
    (void)ctx;
    size_t seen = 0;
    for (size_t i = 0; i < M17_SMS_MAX_MESSAGES; i++) {
        if (!entries[i].active)
            continue;
        if (seen == idx)
            return &entries[i].hdr;
        seen++;
    }
    return nullptr;
}

static uint32_t sms_supported_actions(const message_header_t *hdr)
{
    uint32_t acts = MSG_ACTION_VIEW | MSG_ACTION_DELETE;
    acts |= hdr->unread ? MSG_ACTION_MARK_READ : MSG_ACTION_MARK_UNREAD;
    if (hdr->direction == MSG_DIR_RX)
        acts |= MSG_ACTION_REPLY;
    return acts;
}

static int sms_invoke_action(message_header_t *hdr, message_action_t action)
{
    SmsEntry *e = reinterpret_cast<SmsEntry *>(hdr);

    switch (action) {
        case MSG_ACTION_MARK_READ:
            hdr->unread = false;
            return 0;

        case MSG_ACTION_MARK_UNREAD:
            hdr->unread = true;
            return 0;

        case MSG_ACTION_DELETE:
            e->active = false;
            return 0;

        case MSG_ACTION_REPLY:
            /* The UI sets compose_recipient from hdr->sender directly. */
            return 0;

        case MSG_ACTION_VIEW:
            return 0;

        default:
            return -EINVAL;
    }
}

/**
 * UI-thread tick: drain pkt_rx_event_t events from packet_io.
 *
 * For RX events (status == MSG_STATUS_RECEIVED): allocate a slot and body
 * pool entry, populate the header, set active.
 *
 * For TX completion events (status == SENT or FAILED): find the matching
 * in-flight TX entry by tag and update its status.
 */
static void sms_tick(void *ctx)
{
    (void)ctx;

    /* Static: avoid 847-byte stack allocation on the 2048-byte UI thread. */
    static pkt_rx_event_t evt;
    if (packet_io_dequeue_rx(&evt)) {
        if (evt.mode_id != (uint8_t)OPMODE_M17)
            return;

        if (evt.status == MSG_STATUS_RECEIVED) {
            size_t text_len = evt.body_len;
            if (text_len >= PKT_BODY_MAX_LEN)
                text_len = PKT_BODY_MAX_LEN - 1;

            size_t slot = alloc_slot();
            SmsEntry &e = entries[slot];

            uint16_t off = pool_alloc((uint16_t)text_len);
            memcpy(&body_pool[off], evt.body, text_len);
            body_pool[off + text_len] = '\0';

            memset(&e.hdr, 0, sizeof(e.hdr));
            e.pool_offset = off;
            e.pool_len = (uint16_t)text_len;
            e.tag = 0;

#ifdef CONFIG_RTC
            e.hdr.timestamp = platform_getCurrentTime();
#endif
            e.hdr.direction = MSG_DIR_RX;
            e.hdr.status = MSG_STATUS_RECEIVED;
            e.hdr.unread = true;

            strncpy(e.hdr.sender, evt.src, sizeof(e.hdr.sender) - 1);
            e.hdr.sender[sizeof(e.hdr.sender) - 1] = '\0';

            e.hdr.body = &body_pool[off];
            e.hdr.body_len = text_len;
            e.active = true;

        } else {
            /* TX completion — find by tag. */
            for (size_t i = 0; i < M17_SMS_MAX_MESSAGES; i++) {
                if (entries[i].active && entries[i].tag == evt.tag
                    && entries[i].hdr.direction == MSG_DIR_TX
                    && entries[i].hdr.status == MSG_STATUS_SENDING) {
                    entries[i].hdr.status =
                        static_cast<message_status_t>(evt.status);
                    break;
                }
            }
        }
    }
}

/**
 * UI-thread send: create an inbox entry and enqueue a TX request.
 */
static int sms_send(void *ctx, const char *body, size_t body_len,
                    const char *recipient)
{
    (void)ctx;

    if (body == nullptr || recipient == nullptr || recipient[0] == '\0')
        return -EINVAL;

    /* Use non-zero tag from current timestamp; fall back to 1 on zero. */
    static uint32_t tag_counter = 0;
    tag_counter++;
    if (tag_counter == 0)
        tag_counter = 1;
    uint32_t tag = tag_counter;

    /* Static: avoid ~841-byte stack allocation on the 2048-byte UI thread —
     * same constraint as req/evt in m17_sms_task_rtx(). sms_send() is only
     * called from the UI thread and is never re-entered. */
    static pkt_tx_request_t req;
    req.mode_id = (uint8_t)OPMODE_M17;
    strncpy(req.dst, recipient, sizeof(req.dst) - 1);
    req.dst[sizeof(req.dst) - 1] = '\0';

    if (body_len > PKT_BODY_MAX_LEN - 1)
        return -EMSGSIZE;

    memcpy(req.body, body, body_len);
    req.body[body_len] = '\0';
    req.body_len = body_len;
    req.tag = tag;

    if (!packet_io_enqueue_tx(&req))
        return -EBUSY;

    /* Create the outbox entry on the UI side. */
    size_t slot = alloc_slot();
    SmsEntry &e = entries[slot];

    uint16_t off = pool_alloc((uint16_t)body_len);
    memcpy(&body_pool[off], body, body_len);
    body_pool[off + body_len] = '\0';

    memset(&e.hdr, 0, sizeof(e.hdr));
    e.pool_offset = off;
    e.pool_len = (uint16_t)body_len;
    e.tag = tag;

#ifdef CONFIG_RTC
    e.hdr.timestamp = platform_getCurrentTime();
#endif
    e.hdr.direction = MSG_DIR_TX;
    e.hdr.status = MSG_STATUS_SENDING;
    e.hdr.unread = false;

    rtxStatus_t st = rtx_getCurrentStatus();
    strncpy(e.hdr.sender, st.source_address, sizeof(e.hdr.sender) - 1);
    e.hdr.sender[sizeof(e.hdr.sender) - 1] = '\0';
    strncpy(e.hdr.recipient, recipient, sizeof(e.hdr.recipient) - 1);
    e.hdr.recipient[sizeof(e.hdr.recipient) - 1] = '\0';

    e.hdr.body = &body_pool[off];
    e.hdr.body_len = body_len;
    e.active = true;

    return 0;
}

/* ===================================================================
 * Exported vtable
 * =================================================================== */

const message_type_vtable_t m17_sms_vtable = {
    /* name                */ "M17 SMS",
    /* count               */ sms_count,
    /* get                 */ sms_get,
    /* supported_actions   */ sms_supported_actions,
    /* invoke_action       */ sms_invoke_action,
    /* start_compose       */ NULL,
    /* on_evict            */ NULL,
    /* tick                */ sms_tick,
    /* send                */ sms_send,
    /* mode_id             */ (uint8_t)OPMODE_M17,
};

/* ===================================================================
 * Public API
 * =================================================================== */

void m17_sms_init(void)
{
    memset(entries, 0, sizeof(entries));
    memset(body_pool, 0, sizeof(body_pool));
    pool_head = 0;

    tx_desc.status = PKT_STATUS_IDLE;

    rx_desc.buffer = rx_buf;
    rx_desc.size = sizeof(rx_buf);
    rx_desc.status = PKT_STATUS_IDLE;
}

void m17_sms_task_rtx(void)
{
    /*
     * Arm the RX descriptor whenever it is idle.  Using a retry-on-IDLE loop
     * rather than a one-shot flag handles two cases:
     *   1. The very first call may race before the M17 opmode is active;
     *      rtx_addPacketRx() then resets status to PKT_STATUS_IDLE on failure,
     *      so the next tick retries automatically.
     *   2. After a received packet is processed the descriptor is reset to
     *      PKT_STATUS_IDLE so we re-arm for the next incoming packet.
     */
    if (rx_desc.status == PKT_STATUS_IDLE)
        rtx_addPacketRx(&rx_desc);

    /* rtx_req, rtx_evt, rtx_rx_evt, rtx_tx_evt_ready, rtx_rx_evt_ready,
     * rtx_rx_text are file-scope statics;
     * see the RTX-thread storage section above.
     *
     * rtx_tx_evt_ready: a FAILED or SENT completion event is filled in
     * rtx_evt and waiting to be posted.  We retry posting on each tick
     * until the UI thread drains the RX queue (packet_io_dequeue_rx).
     * rtx_rx_evt_ready: a RECEIVED event is filled in rtx_rx_evt and
     * waiting to be posted similarly. */

    /* Retry posting a pending TX completion event. */
    if (rtx_tx_evt_ready) {
        if (packet_io_enqueue_rx(&rtx_evt)) {
            rtx_tx_evt_ready = false;
            tx_inflight_tag = 0;
        }
        return;
    }

    /* Retry posting a pending RX received event. */
    if (rtx_rx_evt_ready) {
        if (packet_io_enqueue_rx(&rtx_rx_evt))
            rtx_rx_evt_ready = false;
        return;
    }

    /* --- TX: pick up a new request if idle --- */
    if (tx_desc.status == PKT_STATUS_IDLE) {
        if (packet_io_dequeue_tx(&rtx_req)) {
            /* Format the M17 application-layer SMS packet. */
            size_t pkt_len = M17::sms_format_packet(
                rtx_req.body, rtx_req.body_len, tx_buf, sizeof(tx_buf));
            if (pkt_len == 0) {
                /* Format error — post a FAILED completion event. */
                memset(&rtx_evt, 0, sizeof(rtx_evt));
                rtx_evt.mode_id = (uint8_t)OPMODE_M17;
                rtx_evt.tag = rtx_req.tag;
                rtx_evt.status = MSG_STATUS_FAILED;
                if (!packet_io_enqueue_rx(&rtx_evt))
                    rtx_tx_evt_ready = true; /* retry next tick */
            } else {
                tx_inflight_tag = rtx_req.tag;
                tx_desc.buffer = tx_buf;
                tx_desc.size = pkt_len;
                strncpy(tx_desc.destination, rtx_req.dst,
                        sizeof(tx_desc.destination) - 1);
                tx_desc.destination[sizeof(tx_desc.destination) - 1] = '\0';
                rtx_addPacketTx(&tx_desc);
            }
        }
    }

    /* --- TX completion check --- */
    if (tx_desc.status == PKT_STATUS_DONE
        || tx_desc.status == PKT_STATUS_ERROR) {
        uint8_t final_status = (tx_desc.status == PKT_STATUS_DONE) ?
                                   (uint8_t)MSG_STATUS_SENT :
                                   (uint8_t)MSG_STATUS_FAILED;

        memset(&rtx_evt, 0, sizeof(rtx_evt));
        rtx_evt.mode_id = (uint8_t)OPMODE_M17;
        rtx_evt.tag = tx_inflight_tag;
        rtx_evt.status = final_status;
        if (packet_io_enqueue_rx(&rtx_evt)) {
            tx_desc.status = PKT_STATUS_IDLE;
            tx_inflight_tag = 0;
        } else {
            /* RX queue full — retry next tick; leave tx_desc.status as-is. */
            rtx_tx_evt_ready = true;
        }
    }

    /* --- RX completion check --- */
    if (rx_desc.status == PKT_STATUS_ERROR)
    {
        /* Deframer error (bad CRC, sequence, overflow); discard and re-arm. */
        rx_desc.status = PKT_STATUS_IDLE;
        return;
    }
    if (rx_desc.status != PKT_STATUS_DONE)
        return;

    /* Parse the received application-layer payload.  rtx_rx_text is a
     * file-scope static to avoid an 822-byte stack allocation on the
     * embedded RTX thread. */
    size_t pkt_len = (rx_desc.res > 0) ? (size_t)rx_desc.res : 0;

    bool ok =
        M17::sms_parse_packet(static_cast<const uint8_t *>(rx_desc.buffer),
                              pkt_len, rtx_rx_text, sizeof(rtx_rx_text));

    if (ok) {
        size_t text_len = strlen(rtx_rx_text);

        memset(&rtx_rx_evt, 0, sizeof(rtx_rx_evt));
        rtx_rx_evt.mode_id = (uint8_t)OPMODE_M17;
        rtx_rx_evt.tag = 0;
        rtx_rx_evt.status = (uint8_t)MSG_STATUS_RECEIVED;

        size_t copy_len = (text_len < PKT_BODY_MAX_LEN) ?
                              text_len :
                              (PKT_BODY_MAX_LEN - 1);
        memcpy(rtx_rx_evt.body, rtx_rx_text, copy_len);
        rtx_rx_evt.body[copy_len] = '\0';
        rtx_rx_evt.body_len = copy_len;

        /* The source callsign is decoded from the LSF by the M17 layer and
         * stored in rtxStatus_t::M17_src.  Use rtx_getStatus() to read it
         * via pointer rather than copying the full 140-byte struct onto the
         * 512-byte RTX stack. */
        const rtxStatus_t *st = rtx_getStatus();
        strncpy(rtx_rx_evt.src, st->M17_src, sizeof(rtx_rx_evt.src) - 1);
        rtx_rx_evt.src[sizeof(rtx_rx_evt.src) - 1] = '\0';

        if (!packet_io_enqueue_rx(&rtx_rx_evt)) {
            /* Queue full; retry next tick.  rx_desc will not be re-armed
             * until the event is successfully delivered. */
            rtx_rx_evt_ready = true;
            return;
        }

#ifdef PLATFORM_LINUX
        /* Signal reception on stderr so the loopback test script can detect
         * it with a simple grep.  Guarded: fprintf pulls in stdio and uses
         * significant stack — unsafe on embedded RTX thread (512 B stack). */
        fprintf(stderr, "SMS_RECEIVED from '%s': '%s'\n", rtx_rx_evt.src,
                rtx_rx_text);
#endif
    }

    /* Re-arm for the next incoming packet. */
    rx_desc.status = PKT_STATUS_IDLE;
}

#endif /* CONFIG_M17_SMS */
