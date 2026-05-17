/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * m17_sms.cpp — M17 SMS message source for the generic message inbox.
 *
 * Provides a flat pool of SMS entries (M17_SMS_MAX_MESSAGES slots, each
 * holding up to SMS_BODY_MAX bytes of body text).  Entries are created on
 * RX reception and on TX submission; the oldest entry is evicted silently
 * when the pool is full.
 *
 * The vtable m17_sms_vtable is referenced by the static source table in
 * messages.cpp — no runtime registration is required.
 *
 * Thread safety: m17_sms_task() is called from the RTX thread.
 * m17_sms_send() is called from the UI thread.
 * vtable callbacks are called from the UI thread via messages_tick().
 * Access to entries[] is lock-free because the only cross-thread hazard is
 * the status field of a TX entry, which is promoted atomically from
 * MSG_STATUS_SENDING to SENT/FAILED only in m17_sms_task().
 */

#include "hwconfig.h"

#ifdef CONFIG_M17

#include "core/m17_sms.h"
#include "interfaces/platform.h"
#include "protocols/M17/SmsPacket.hpp"
#include "rtx/rtx.h"

#include <cerrno>
#ifdef PLATFORM_LINUX
#include <cstdio>
#endif
#include <cstdint>
#include <cstring>

/* Maximum body text bytes stored per entry (includes NUL terminator). */
#define SMS_BODY_MAX (M17_SMS_POOL_BYTES / M17_SMS_MAX_MESSAGES)

/* M17 packet data buffer size — matches PacketFramer/Deframer max payload. */
#define SMS_PKT_BUF 825

/* -------------------------------------------------------------------
 * Per-entry storage
 * ----------------------------------------------------------------- */

/**
 * One stored SMS.  The message_header_t MUST be the first member so that
 * a vtable get() return value can be reinterpret_cast'd back to SmsEntry.
 */
struct SmsEntry {
    message_header_t hdr;    /**< Registry-visible header. Must be first. */
    char body[SMS_BODY_MAX]; /**< Body text (NUL-terminated). */
    bool active;             /**< Slot is in use. */
};

static SmsEntry entries[M17_SMS_MAX_MESSAGES];

/* -------------------------------------------------------------------
 * TX / RX packet descriptors
 * ----------------------------------------------------------------- */

static uint8_t tx_buf[SMS_PKT_BUF];
static struct pktDesc tx_desc;
static size_t tx_slot; /* entries[] index of the in-flight TX entry */

static uint8_t rx_buf[SMS_PKT_BUF];
static struct pktDesc rx_desc;

/* -------------------------------------------------------------------
 * Compose state
 * ----------------------------------------------------------------- */

static bool compose_pending_flag;
static char compose_recipient_buf[16]; /* matches message_header_t::sender */

/* -------------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------- */

/**
 * Compare two datetime_t values.
 * @return negative if a < b, 0 if equal, positive if a > b.
 */
static int datetime_cmp(const datetime_t &a, const datetime_t &b)
{
    if (a.year != b.year)
        return (int)(uint8_t)a.year - (int)(uint8_t)b.year;
    if (a.month != b.month)
        return (int)a.month - (int)b.month;
    if (a.date != b.date)
        return (int)a.date - (int)b.date;
    if (a.hour != b.hour)
        return (int)a.hour - (int)b.hour;
    if (a.minute != b.minute)
        return (int)a.minute - (int)b.minute;
    return (int)a.second - (int)b.second;
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
        if (datetime_cmp(entries[i].hdr.timestamp,
                         entries[oldest].hdr.timestamp)
            < 0) {
            oldest = i;
        }
    }

    entries[oldest].active = false;
    return oldest;
}

/* -------------------------------------------------------------------
 * Vtable callbacks
 * ----------------------------------------------------------------- */

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
            compose_pending_flag = true;
            strncpy(compose_recipient_buf, hdr->sender,
                    sizeof(compose_recipient_buf) - 1);
            compose_recipient_buf[sizeof(compose_recipient_buf) - 1] = '\0';
            return 0;

        case MSG_ACTION_VIEW:
            return 0;

        default:
            return -EINVAL;
    }
}

static void sms_start_compose(void *ctx)
{
    (void)ctx;
    compose_pending_flag = true;
    compose_recipient_buf[0] = '\0';
}

/* -------------------------------------------------------------------
 * Exported vtable
 * ----------------------------------------------------------------- */

const message_type_vtable_t m17_sms_vtable = {
    /* name              */ "M17 SMS",
    /* count             */ sms_count,
    /* get               */ sms_get,
    /* render_list_row   */ NULL,
    /* render_detail     */ NULL,
    /* handle_detail_input */ NULL,
    /* supported_actions */ sms_supported_actions,
    /* invoke_action     */ sms_invoke_action,
    /* start_compose     */ sms_start_compose,
    /* on_evict          */ NULL,
    /* mode_id           */ 3, /* OPMODE_M17 */
};

/* -------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------- */

void m17_sms_init(void)
{
    memset(entries, 0, sizeof(entries));
    compose_pending_flag = false;
    compose_recipient_buf[0] = '\0';

    tx_desc.status = PKT_STATUS_IDLE;

    rx_desc.buffer = rx_buf;
    rx_desc.size = sizeof(rx_buf);
    rx_desc.status = PKT_STATUS_IDLE;
}

void m17_sms_task(void)
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

    /* --- TX completion check --- */
    if (tx_desc.status == PKT_STATUS_DONE) {
        entries[tx_slot].hdr.status = MSG_STATUS_SENT;
        tx_desc.status = PKT_STATUS_IDLE;
    } else if (tx_desc.status == PKT_STATUS_ERROR) {
        entries[tx_slot].hdr.status = MSG_STATUS_FAILED;
        tx_desc.status = PKT_STATUS_IDLE;
    }

    /* --- RX completion check --- */
    if (rx_desc.status != PKT_STATUS_DONE)
        return;

    /* Parse the received application-layer payload. */
    char text[SMS_BODY_MAX];
    size_t pkt_len = (rx_desc.res > 0) ? (size_t)rx_desc.res : 0;

    bool ok =
        M17::sms_parse_packet(static_cast<const uint8_t *>(rx_desc.buffer),
                              pkt_len, text, sizeof(text));

    if (ok) {
        size_t slot = alloc_slot();
        SmsEntry &e = entries[slot];

        memset(&e, 0, sizeof(e));
        strncpy(e.body, text, SMS_BODY_MAX - 1);
        e.body[SMS_BODY_MAX - 1] = '\0';

#ifdef CONFIG_RTC
        e.hdr.timestamp = platform_getCurrentTime();
#endif
        e.hdr.direction = MSG_DIR_RX;
        e.hdr.status = MSG_STATUS_RECEIVED;
        e.hdr.unread = true;

        /* The source callsign is decoded from the LSF by the M17 layer and
         * stored in rtxStatus_t::M17_src. */
        rtxStatus_t st = rtx_getCurrentStatus();
        strncpy(e.hdr.sender, st.M17_src, sizeof(e.hdr.sender) - 1);
        e.hdr.sender[sizeof(e.hdr.sender) - 1] = '\0';

        e.hdr.body = e.body;
        e.hdr.body_len = strlen(e.body);
        e.active = true;

#ifdef PLATFORM_LINUX
        /* Signal reception on stderr so the loopback test script can detect
         * it with a simple grep.  Guarded: fprintf pulls in stdio and uses
         * significant stack — unsafe on embedded RTX thread (512 B stack). */
        fprintf(stderr, "SMS_RECEIVED from '%s': '%s'\n", e.hdr.sender, e.body);
#endif
    }

    /* Re-arm for the next incoming packet. */
    rx_desc.status = PKT_STATUS_IDLE;
}

int m17_sms_send(const char *message, size_t msgLen, const char *recipient)
{
    if (message == nullptr || recipient == nullptr || recipient[0] == '\0')
        return -EINVAL;

    /* Reject if a TX packet is already in flight. */
    if (tx_desc.status == PKT_STATUS_SUBMITTED)
        return -EBUSY;

    /* Format the M17 application-layer SMS packet. */
    size_t pkt_len = M17::sms_format_packet(message, msgLen, tx_buf,
                                            sizeof(tx_buf));
    if (pkt_len == 0)
        return -EMSGSIZE;

    /* Allocate a storage slot (evicts oldest if pool is full). */
    size_t slot = alloc_slot();
    SmsEntry &e = entries[slot];

    memset(&e, 0, sizeof(e));
    size_t copy_len = (msgLen < (size_t)(SMS_BODY_MAX - 1)) ?
                          msgLen :
                          (size_t)(SMS_BODY_MAX - 1);
    memcpy(e.body, message, copy_len);
    e.body[copy_len] = '\0';

#ifdef CONFIG_RTC
    e.hdr.timestamp = platform_getCurrentTime();
#endif
    e.hdr.direction = MSG_DIR_TX;
    e.hdr.status = MSG_STATUS_SENDING;
    e.hdr.unread = false;

    /* Local callsign sourced from the current RTX configuration. */
    rtxStatus_t st = rtx_getCurrentStatus();
    strncpy(e.hdr.sender, st.source_address, sizeof(e.hdr.sender) - 1);
    e.hdr.sender[sizeof(e.hdr.sender) - 1] = '\0';
    strncpy(e.hdr.recipient, recipient, sizeof(e.hdr.recipient) - 1);
    e.hdr.recipient[sizeof(e.hdr.recipient) - 1] = '\0';

    e.hdr.body = e.body;
    e.hdr.body_len = copy_len;
    e.active = true;

    /* Submit the TX packet descriptor. */
    tx_desc.buffer = tx_buf;
    tx_desc.size = pkt_len;
    tx_desc.status = PKT_STATUS_IDLE;
    strncpy(tx_desc.destination, recipient, sizeof(tx_desc.destination) - 1);
    tx_desc.destination[sizeof(tx_desc.destination) - 1] = '\0';
    tx_slot = slot;

    int ret = rtx_addPacketTx(&tx_desc);
    if (ret != 0)
        e.hdr.status = MSG_STATUS_FAILED;

    return ret;
}

bool m17_sms_compose_pending(void)
{
    return compose_pending_flag;
}

const char *m17_sms_compose_recipient(void)
{
    return compose_recipient_buf;
}

void m17_sms_compose_clear(void)
{
    compose_pending_flag = false;
    compose_recipient_buf[0] = '\0';
}

#endif /* CONFIG_M17 */
