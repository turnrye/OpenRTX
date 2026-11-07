/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "core/messages.h"
#include "core/utils.h"
#include "rtx/rtx.h"

/* Maximum number of entries held in the inbox. */
#ifndef CONFIG_MESSAGES_MAX_ENTRIES
#define CONFIG_MESSAGES_MAX_ENTRIES 32
#endif

/* Size of the shared body pool, in bytes. */
#ifndef CONFIG_MESSAGES_POOL_BYTES
#define CONFIG_MESSAGES_POOL_BYTES 3200
#endif

/* Maximum number of protocol sources that can be registered. */
#ifndef CONFIG_MESSAGES_MAX_SOURCES
#define CONFIG_MESSAGES_MAX_SOURCES 2
#endif

/* Number of receive descriptors kept submitted to the rtx stage. */
#ifndef CONFIG_MESSAGES_RX_SLOTS
#define CONFIG_MESSAGES_RX_SLOTS 1
#endif

_Static_assert(CONFIG_MESSAGES_MAX_ENTRIES > 0,
               "CONFIG_MESSAGES_MAX_ENTRIES must be positive");
_Static_assert(CONFIG_MESSAGES_POOL_BYTES >= MSG_BODY_MAX_LEN,
               "CONFIG_MESSAGES_POOL_BYTES must hold at least one full body");

/**
 * \internal
 * A packet descriptor with its buffer and the operating mode it was
 * submitted in, which selects the source that handles it on completion.
 */
struct pktSlot {
    struct pktDesc desc;
    uint8_t mode;
    uint8_t buffer[MSG_PKT_MAX_SIZE];
};

/**
 * \internal
 * Everything the inbox needs while active, allocated as a single block.
 *
 * The entry table is a ring: head is the position of the oldest entry and
 * new entries are appended after the newest, so evicting the oldest is just
 * advancing head. Bodies live in the pool as NUL-terminated strings
 * allocated in arrival order: poolHead is the next byte to write and wraps
 * to zero when a body would not fit at the end.
 */
struct messageCtx {
    size_t head;
    size_t numEntries;
    size_t newArrivals;
    size_t poolHead;
    uint32_t txSequence;
    struct message entries[CONFIG_MESSAGES_MAX_ENTRIES];
    char pool[CONFIG_MESSAGES_POOL_BYTES];
    struct pktSlot rxSlots[CONFIG_MESSAGES_RX_SLOTS];
    struct pktSlot txSlot;
};

/*
 * Module state. The internal helpers below never reach for these directly:
 * the storage (ctx) and the source table (sources/numSources) are passed in
 * as the first parameters, so each helper's signature says what global state
 * it works on. Only the public API, which owns this state, names it.
 */
static const struct messageOps *sources[CONFIG_MESSAGES_MAX_SOURCES];
static size_t numSources;
static struct messageCtx *ctx;
static uint32_t nextSequence;

/**
 * \internal
 * Get the source registered for an operating mode, NULL if none.
 *
 * @param sources: table of registered sources.
 * @param numSources: number of entries in the table.
 * @param mode: operating mode to look up.
 */
static const struct messageOps *findSource(const struct messageOps **sources,
                                           size_t numSources, uint8_t mode)
{
    for (size_t i = 0; i < numSources; i++) {
        if (sources[i]->mode == mode)
            return sources[i];
    }

    return NULL;
}

/**
 * \internal
 * Get an entry from its age: position 0 is the oldest entry stored,
 * position numEntries - 1 the newest.
 *
 * @param ctx: inbox storage.
 * @param pos: position of the entry, oldest first.
 * @return pointer to the entry.
 */
static inline struct message *entryAt(struct messageCtx *ctx, size_t pos)
{
    return &ctx->entries[(ctx->head + pos) % CONFIG_MESSAGES_MAX_ENTRIES];
}

/**
 * \internal
 * Remove an entry, keeping the remaining entries in order. Removing the
 * oldest entry, the only case eviction ever needs, just advances the ring
 * head; removing a newer one shifts the entries behind it.
 *
 * @param ctx: inbox storage.
 * @param pos: position of the entry, oldest first.
 */
static void removeEntry(struct messageCtx *ctx, size_t pos)
{
    if (pos == 0) {
        ctx->head = (ctx->head + 1) % CONFIG_MESSAGES_MAX_ENTRIES;
    } else {
        for (size_t i = pos; i + 1 < ctx->numEntries; i++)
            *entryAt(ctx, i) = *entryAt(ctx, i + 1);
    }

    ctx->numEntries--;
}

/**
 * \internal
 * Pick the entry to evict when the table is full: the oldest one that is not
 * still waiting to be transmitted. A message in flight is only lost once
 * everything else has been.
 *
 * @param ctx: inbox storage.
 * @return position of the entry to remove, oldest first.
 */
static size_t evictable(struct messageCtx *ctx)
{
    for (size_t i = 0; i < ctx->numEntries; i++) {
        const struct message *entry = entryAt(ctx, i);

        if ((entry->direction != MSG_DIR_TX)
            || (entry->status != MSG_STATUS_SENDING))
            return i;
    }

    return 0;
}

/**
 * \internal
 * Check whether any stored body overlaps a range of the pool.
 *
 * @param ctx: inbox storage.
 * @param start: first byte of the range.
 * @param end: one past the last byte of the range.
 * @return true if some body overlaps the range.
 */
static bool poolInUse(struct messageCtx *ctx, size_t start, size_t end)
{
    for (size_t i = 0; i < ctx->numEntries; i++) {
        const struct message *entry = entryAt(ctx, i);
        size_t bodyStart = entry->body - ctx->pool;
        size_t bodyEnd = bodyStart + entry->bodyLen + 1;

        if ((bodyStart < end) && (bodyEnd > start))
            return true;
    }

    return false;
}

/**
 * \internal
 * Reserve room in the pool for a body plus NUL, evicting the oldest entries
 * until nothing overlaps the reserved range. The pool is written in arrival
 * order, so the overlapping bodies are among the oldest, but after a wrap an
 * older body may survive at the end of the pool: evicting in age order
 * keeps eviction strictly oldest-first at the cost of an entry or two.
 *
 * @param ctx: inbox storage.
 * @param len: body length, not counting the NUL.
 * @return pool offset of the reserved range.
 */
static size_t poolAlloc(struct messageCtx *ctx, size_t len)
{
    size_t need = len + 1;

    if (ctx->poolHead + need > CONFIG_MESSAGES_POOL_BYTES)
        ctx->poolHead = 0;

    size_t start = ctx->poolHead;
    size_t end = ctx->poolHead + need;

    while (poolInUse(ctx, start, end))
        removeEntry(ctx, 0);

    ctx->poolHead = end;
    return start;
}

/**
 * \internal
 * Allocate and reset the storage. Returns NULL if the heap is exhausted; the
 * next task run tries again.
 *
 * @return the freshly allocated storage, or NULL on allocation failure.
 */
static struct messageCtx *allocate(void)
{
    struct messageCtx *storage = malloc(sizeof(*storage));
    if (storage == NULL)
        return NULL;

    // Zeroes every counter and sets each descriptor to PKT_STATUS_IDLE,
    // which is zero.
    memset(storage, 0, sizeof(*storage));
    return storage;
}

/**
 * \internal
 * Check whether the rtx stage still holds any packet descriptor: the storage
 * cannot be released before every one has been handed back.
 *
 * @param ctx: inbox storage.
 */
static bool slotsBusy(struct messageCtx *ctx)
{
    for (size_t i = 0; i < ARRAY_SIZE(ctx->rxSlots); i++) {
        if (ctx->rxSlots[i].desc.status == PKT_STATUS_SUBMITTED)
            return true;
    }

    return ctx->txSlot.desc.status == PKT_STATUS_SUBMITTED;
}

/**
 * \internal
 * Drive one receive slot: hand a completed packet to its source, then keep
 * the slot submitted while a source is registered for the current mode.
 *
 * @param sources: table of registered sources.
 * @param numSources: number of entries in the table.
 * @param slot: receive slot to drive.
 * @param ops: source registered for the current mode, NULL if none.
 * @param mode: current operating mode.
 */
static void handleRx(const struct messageOps **sources, size_t numSources,
                     struct pktSlot *slot, const struct messageOps *ops,
                     uint8_t mode)
{
    const struct messageOps *owner;
    int ret;

    switch (slot->desc.status) {
        case PKT_STATUS_DONE:
            owner = findSource(sources, numSources, slot->mode);
            ret = (owner != NULL) ? owner->processRx(&slot->desc) : -EPROTO;

            /* Not the mode's own protocol: another source may carry it. */
            for (size_t i = 0; (ret == -EPROTO) && (i < numSources); i++) {
                if ((sources[i] != owner) && (sources[i]->processGuest != NULL))
                    ret = sources[i]->processGuest(slot->mode, &slot->desc);
            }

            /* fallthrough */
        case PKT_STATUS_ERROR:
            slot->desc.status = PKT_STATUS_IDLE;

            /* fallthrough */
        case PKT_STATUS_IDLE:
            if (ops == NULL)
                break;

            slot->desc.buffer = slot->buffer;
            slot->desc.size = sizeof(slot->buffer);
            slot->mode = mode;
            rtx_addPacketRx(&slot->desc);
            break;

        case PKT_STATUS_SUBMITTED:
            break;
    }
}

/**
 * \internal
 * Move an entry out of MSG_STATUS_SENDING. A retry or a packet that belongs to
 * no entry leaves every status alone, as does an entry a source already
 * settled, e.g. acknowledged while its transmission was still completing.
 *
 * @param ctx: inbox storage.
 * @param sequence: sequence number of the entry.
 * @param status: status to settle the entry on.
 */
static void settle(struct messageCtx *ctx, uint32_t sequence,
                   enum messageStatus status)
{
    size_t idx = messages_findBySequence(sequence);

    if (idx == SIZE_MAX)
        return;

    struct message *entry = entryAt(ctx, ctx->numEntries - idx - 1);
    if (entry->status == MSG_STATUS_SENDING)
        entry->status = status;
}

/**
 * \internal
 * Report the outcome of a completed transmission to its entry and free the
 * transmit slot.
 *
 * @param ctx: inbox storage.
 */
static void handleTx(struct messageCtx *ctx)
{
    switch (ctx->txSlot.desc.status) {
        case PKT_STATUS_DONE:
            settle(ctx, ctx->txSequence, MSG_STATUS_SENT);
            ctx->txSlot.desc.status = PKT_STATUS_IDLE;
            break;

        case PKT_STATUS_ERROR:
            settle(ctx, ctx->txSequence, MSG_STATUS_FAILED);
            ctx->txSlot.desc.status = PKT_STATUS_IDLE;
            break;

        case PKT_STATUS_IDLE:
        case PKT_STATUS_SUBMITTED:
            break;
    }
}

/**
 * \internal
 * Transmit a stored message through the source registered for its operating
 * mode. The outcome is reported asynchronously by handleTx(); -EBUSY means
 * the transmitter is taken and the message can be tried again later.
 *
 * @param ctx: inbox storage.
 * @param sources: table of registered sources.
 * @param numSources: number of entries in the table.
 * @param msg: stored message to transmit.
 */
static int transmit(struct messageCtx *ctx, const struct messageOps **sources,
                    size_t numSources, const struct message *msg)
{
    const struct messageOps *ops = findSource(sources, numSources, msg->mode);

    if (ctx->txSlot.desc.status != PKT_STATUS_IDLE)
        return -EBUSY;

    ctx->txSlot.desc.buffer = ctx->txSlot.buffer;
    ctx->txSlot.desc.size = sizeof(ctx->txSlot.buffer);

    int ret = ops->formatTx(msg, &ctx->txSlot.desc);
    if (ret != 0)
        return ret;

    ctx->txSlot.mode = msg->mode;
    ctx->txSequence = msg->sequence;

    return rtx_addPacketTx(&ctx->txSlot.desc);
}

/**
 * \internal
 * Submit the oldest message still waiting for the transmitter, if any. A
 * waiting message is one in MSG_STATUS_SENDING while the slot is free.
 *
 * @param ctx: inbox storage.
 * @param sources: table of registered sources.
 * @param numSources: number of entries in the table.
 * @param mode: current operating mode.
 */
static void sendWaiting(struct messageCtx *ctx,
                        const struct messageOps **sources, size_t numSources,
                        uint8_t mode)
{
    if (ctx->txSlot.desc.status != PKT_STATUS_IDLE)
        return;

    for (size_t i = 0; i < ctx->numEntries; i++) {
        struct message *entry = entryAt(ctx, i);

        /* A message for another mode waits for it: this mode's opmode
         * would take its packet for one of its own. */
        if ((entry->direction != MSG_DIR_TX)
            || (entry->status != MSG_STATUS_SENDING)
            || (messages_travelMode(entry) != mode))
            continue;

        int ret = transmit(ctx, sources, numSources, entry);
        if ((ret != 0) && (ret != -EBUSY))
            entry->status = MSG_STATUS_FAILED;

        return;
    }
}

void messages_init(void)
{
    memset(sources, 0, sizeof(sources));
    numSources = 0;
    nextSequence = 1;
    free(ctx);
    ctx = NULL;
}

void messages_terminate(void)
{
    if ((ctx != NULL) && slotsBusy(ctx))
        return;

    free(ctx);
    ctx = NULL;
}

int messages_registerSource(const struct messageOps *ops)
{
    if ((ops == NULL) || (ops->processRx == NULL) || (ops->formatTx == NULL))
        return -EINVAL;

    if (findSource(sources, numSources, ops->mode) != NULL)
        return -EEXIST;

    if (numSources == ARRAY_SIZE(sources))
        return -ENOSPC;

    sources[numSources++] = ops;
    return 0;
}

size_t messages_task(uint8_t mode)
{
    const struct messageOps *ops = findSource(sources, numSources, mode);

    if (ctx == NULL) {
        if (ops == NULL)
            return 0;

        ctx = allocate();
        if (ctx == NULL)
            return 0;
    }

    for (size_t i = 0; i < ARRAY_SIZE(ctx->rxSlots); i++)
        handleRx(sources, numSources, &ctx->rxSlots[i], ops, mode);

    handleTx(ctx);

    /*
     * No source for the current mode: release the storage as soon as the
     * rtx stage has handed back every descriptor, dropping the content.
     */
    if (ops == NULL) {
        messages_terminate();
        return 0;
    }

    /* The sources go first: their acknowledgements are time critical. */
    if (ops->task != NULL)
        ops->task();

    for (size_t i = 0; i < numSources; i++) {
        if ((sources[i] != ops) && (sources[i]->guestTask != NULL))
            sources[i]->guestTask(mode);
    }

    sendWaiting(ctx, sources, numSources, mode);

    size_t arrivals = ctx->newArrivals;
    ctx->newArrivals = 0;
    return arrivals;
}

size_t messages_count(void)
{
    if (ctx == NULL)
        return 0;

    return ctx->numEntries;
}

size_t messages_countUnread(void)
{
    size_t unread = 0;

    for (size_t i = 0; i < messages_count(); i++) {
        if (entryAt(ctx, i)->unread != 0)
            unread++;
    }

    return unread;
}

const struct message *messages_get(size_t idx)
{
    if (idx >= messages_count())
        return NULL;

    return entryAt(ctx, ctx->numEntries - idx - 1);
}

size_t messages_findBySequence(uint32_t sequence)
{
    for (size_t i = 0; i < messages_count(); i++) {
        if (entryAt(ctx, i)->sequence == sequence)
            return ctx->numEntries - i - 1;
    }

    return SIZE_MAX;
}

int32_t messages_store(const struct message *msg)
{
    if ((msg == NULL) || ((msg->body == NULL) && (msg->bodyLen != 0)))
        return -EINVAL;

    if (msg->bodyLen >= MSG_BODY_MAX_LEN)
        return -EMSGSIZE;

    if (ctx == NULL)
        return -ENODEV;

    if (ctx->numEntries == ARRAY_SIZE(ctx->entries))
        removeEntry(ctx, evictable(ctx));

    size_t offset = poolAlloc(ctx, msg->bodyLen);
    if (msg->bodyLen > 0)
        memcpy(&ctx->pool[offset], msg->body, msg->bodyLen);
    ctx->pool[offset + msg->bodyLen] = '\0';

    struct message *entry = entryAt(ctx, ctx->numEntries);
    ctx->numEntries++;
    *entry = *msg;
    entry->body = &ctx->pool[offset];
    entry->sequence = nextSequence++;
    entry->sender[MSG_ADDR_MAX_LEN - 1] = '\0';
    entry->recipient[MSG_ADDR_MAX_LEN - 1] = '\0';

    if ((entry->direction == MSG_DIR_RX) && (entry->unread != 0))
        ctx->newArrivals++;

    return (int32_t)entry->sequence;
}

int messages_setStatus(uint32_t sequence, enum messageStatus status)
{
    size_t idx = messages_findBySequence(sequence);
    if (idx == SIZE_MAX)
        return -ENOENT;

    entryAt(ctx, ctx->numEntries - idx - 1)->status = status;
    return 0;
}

int messages_markRead(size_t idx, bool read)
{
    if (idx >= messages_count())
        return -ENOENT;

    entryAt(ctx, ctx->numEntries - idx - 1)->unread = read ? 0 : 1;
    return 0;
}

int messages_delete(size_t idx)
{
    if (idx >= messages_count())
        return -ENOENT;

    removeEntry(ctx, ctx->numEntries - idx - 1);
    return 0;
}

bool messages_canCompose(uint8_t mode)
{
    return (findSource(sources, numSources, mode) != NULL) && (ctx != NULL);
}

const char *messages_broadcastAddr(uint8_t mode)
{
    const struct messageOps *ops = findSource(sources, numSources, mode);

    return (ops != NULL) ? ops->broadcastAddr : NULL;
}

size_t messages_split(const char *text, size_t len, size_t limit, size_t *skip)
{
    size_t lead = 0;
    while ((lead < len) && (text[lead] == ' '))
        lead++;
    *skip = lead;
    text += lead;
    len -= lead;

    if (len <= limit) {
        while ((len > 0) && (text[len - 1] == ' '))
            len--;
        return len;
    }

    /* Break at the last space that fits, keeping it out of both pieces:
     * the next call skips it. */
    for (size_t i = limit; i > 0; i--) {
        if (text[i] == ' ')
            return i;
    }

    return limit;
}

size_t messages_pieceLimit(uint8_t mode)
{
    const struct messageOps *src = findSource(sources, numSources, mode);

    if (src == NULL)
        return 0;

    return (src->maxBodyLen != 0) ? src->maxBodyLen : MSG_BODY_MAX_LEN - 1;
}

size_t messages_pieces(uint8_t mode, const char *text, size_t len,
                       size_t cursor, size_t *current)
{
    size_t limit = messages_pieceLimit(mode);
    size_t count = 0;
    size_t pos = 0;
    size_t skip;
    size_t n;

    if (limit == 0)
        limit = MSG_BODY_MAX_LEN - 1;

    *current = 1;
    while ((n = messages_split(text + pos, len - pos, limit, &skip)) > 0) {
        count++;
        pos += skip + n;
        if (cursor >= pos)
            *current = count + 1;
    }

    if (count == 0)
        return 1;

    if (*current > count)
        *current = count;

    return count;
}

int messages_sendSplit(const struct message *msg)
{
    if ((msg == NULL) || ((msg->body == NULL) && (msg->bodyLen != 0)))
        return -EINVAL;

    size_t limit = messages_pieceLimit(msg->mode);
    if (limit == 0)
        return -ENOENT;

    struct message piece = *msg;
    const char *text = msg->body;
    size_t left = msg->bodyLen;
    size_t skip;
    size_t n;
    int ret = 0;

    while ((ret == 0) && ((n = messages_split(text, left, limit, &skip)) > 0)) {
        piece.body = text + skip;
        piece.bodyLen = (uint16_t)n;
        ret = messages_send(&piece);
        text += skip + n;
        left -= skip + n;
    }

    return ret;
}

uint8_t messages_travelMode(const struct message *msg)
{
    return (msg->carrier != 0) ? msg->carrier : msg->mode;
}

int messages_send(const struct message *msg)
{
    if (msg == NULL)
        return -EINVAL;

    if (findSource(sources, numSources, msg->mode) == NULL)
        return -ENOENT;

    if (ctx == NULL)
        return -ENODEV;

    struct message entry = *msg;
    entry.direction = MSG_DIR_TX;
    entry.status = MSG_STATUS_SENDING;
    entry.unread = 0;

    int32_t seq = messages_store(&entry);
    if (seq < 0)
        return seq;

    /* The entry just stored is the newest one. A busy transmitter is not a
     * failure: the entry waits in MSG_STATUS_SENDING for messages_task(). */
    struct message *stored = entryAt(ctx, ctx->numEntries - 1);
    int ret = transmit(ctx, sources, numSources, stored);
    if (ret == -EBUSY)
        return 0;

    if (ret != 0)
        stored->status = MSG_STATUS_FAILED;

    return ret;
}

int messages_sendPacket(uint8_t mode, const void *pkt, size_t len)
{
    if ((pkt == NULL) || (len == 0))
        return -EINVAL;

    if (len > MSG_PKT_MAX_SIZE)
        return -EMSGSIZE;

    if (ctx == NULL)
        return -ENODEV;

    if (ctx->txSlot.desc.status != PKT_STATUS_IDLE)
        return -EBUSY;

    memcpy(ctx->txSlot.buffer, pkt, len);
    ctx->txSlot.desc.buffer = ctx->txSlot.buffer;
    ctx->txSlot.desc.size = len;
    ctx->txSlot.mode = mode;
    ctx->txSequence = 0;

    return rtx_addPacketTx(&ctx->txSlot.desc);
}

int messages_resend(uint32_t sequence)
{
    size_t idx = messages_findBySequence(sequence);

    if (idx == SIZE_MAX)
        return -ENOENT;

    const struct message *entry = messages_get(idx);
    if (entry->direction != MSG_DIR_TX)
        return -EINVAL;

    return transmit(ctx, sources, numSources, entry);
}
