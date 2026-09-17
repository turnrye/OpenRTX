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

/* Number of receive descriptors kept submitted to the rtx stage. */
#ifndef CONFIG_MESSAGES_RX_SLOTS
#define CONFIG_MESSAGES_RX_SLOTS 1
#endif

_Static_assert(CONFIG_MESSAGES_MAX_ENTRIES > 0,
               "CONFIG_MESSAGES_MAX_ENTRIES must be positive");
_Static_assert(CONFIG_MESSAGES_POOL_BYTES >= MSG_BODY_MAX_LEN,
               "CONFIG_MESSAGES_POOL_BYTES must hold at least one full body");
_Static_assert(CONFIG_MESSAGES_POOL_BYTES <= UINT16_MAX,
               "CONFIG_MESSAGES_POOL_BYTES must fit in a uint16_t offset");

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
 * Entries are kept oldest-first, so entries[0] is the eviction candidate and
 * entries[numEntries - 1] is the newest. Bodies live in the pool as
 * NUL-terminated strings allocated in arrival order: poolHead is the next
 * byte to write and wraps to zero when a body would not fit at the end.
 */
struct messageCtx {
    size_t numEntries;
    size_t newArrivals;
    uint32_t txSequence;
    uint16_t poolHead;
    struct message entries[CONFIG_MESSAGES_MAX_ENTRIES];
    char pool[CONFIG_MESSAGES_POOL_BYTES];
    struct pktSlot rxSlots[CONFIG_MESSAGES_RX_SLOTS];
    struct pktSlot txSlot;
};

static const struct messageOps *sources[CONFIG_MESSAGES_MAX_SOURCES];
static size_t numSources;
static struct messageCtx *ctx;
static uint32_t nextSequence;

/**
 * \internal
 * Get the source registered for an operating mode, NULL if none.
 */
static const struct messageOps *findSource(uint8_t mode)
{
    for (size_t i = 0; i < numSources; i++) {
        if (sources[i]->mode == mode)
            return sources[i];
    }

    return NULL;
}

/**
 * \internal
 * Remove entry @p pos, keeping the remaining entries in order.
 */
static void removeEntry(size_t pos)
{
    size_t tail = ctx->numEntries - pos - 1;

    memmove(&ctx->entries[pos], &ctx->entries[pos + 1],
            tail * sizeof(ctx->entries[0]));
    ctx->numEntries--;
}

/**
 * \internal
 * Reserve room in the pool for a body of @p len characters plus NUL,
 * evicting every entry whose body overlaps the reserved range. Those entries
 * are always older than any other one still stored, since the pool is
 * written in arrival order.
 *
 * @return pool offset of the reserved range.
 */
static uint16_t poolAlloc(uint16_t len)
{
    uint16_t need = len + 1;

    if ((size_t)ctx->poolHead + need > CONFIG_MESSAGES_POOL_BYTES)
        ctx->poolHead = 0;

    uint16_t start = ctx->poolHead;
    uint16_t end = ctx->poolHead + need;

    for (size_t i = 0; i < ctx->numEntries;) {
        uint16_t bodyStart = ctx->entries[i].body - ctx->pool;
        uint16_t bodyEnd = bodyStart + ctx->entries[i].bodyLen + 1;

        if ((bodyStart < end) && (bodyEnd > start))
            removeEntry(i);
        else
            i++;
    }

    ctx->poolHead = end;
    return start;
}

/**
 * \internal
 * Allocate and reset the storage. Fails silently if the heap is exhausted:
 * the next task run tries again.
 */
static void allocate(void)
{
    ctx = malloc(sizeof(*ctx));
    if (ctx == NULL)
        return;

    ctx->numEntries = 0;
    ctx->newArrivals = 0;
    ctx->txSequence = 0;
    ctx->poolHead = 0;

    for (size_t i = 0; i < ARRAY_SIZE(ctx->rxSlots); i++)
        ctx->rxSlots[i].desc.status = PKT_STATUS_IDLE;

    ctx->txSlot.desc.status = PKT_STATUS_IDLE;
}

/**
 * \internal
 * Check whether the rtx stage still holds any packet descriptor: the storage
 * cannot be released before every one has been handed back.
 */
static bool slotsBusy(void)
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
 */
static void handleRx(struct pktSlot *slot, const struct messageOps *ops,
                     uint8_t mode)
{
    const struct messageOps *owner;

    switch (slot->desc.status) {
        case PKT_STATUS_DONE:
            owner = findSource(slot->mode);
            if (owner != NULL)
                owner->processRx(&slot->desc);

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
 * Report the outcome of a completed transmission to its entry and free the
 * transmit slot.
 */
static void handleTx(void)
{
    switch (ctx->txSlot.desc.status) {
        case PKT_STATUS_DONE:
            messages_setStatus(ctx->txSequence, MSG_STATUS_SENT);
            ctx->txSlot.desc.status = PKT_STATUS_IDLE;
            break;

        case PKT_STATUS_ERROR:
            messages_setStatus(ctx->txSequence, MSG_STATUS_FAILED);
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
 * mode. The outcome is reported asynchronously by handleTx().
 */
static int transmit(const struct message *msg)
{
    const struct messageOps *ops = findSource(msg->mode);

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

void messages_init(void)
{
    memset(sources, 0, sizeof(sources));
    numSources = 0;
    nextSequence = 1;
    messages_terminate();
}

void messages_terminate(void)
{
    free(ctx);
    ctx = NULL;
}

int messages_registerSource(const struct messageOps *ops)
{
    if ((ops == NULL) || (ops->processRx == NULL) || (ops->formatTx == NULL))
        return -EINVAL;

    if (findSource(ops->mode) != NULL)
        return -EEXIST;

    if (numSources == ARRAY_SIZE(sources))
        return -ENOSPC;

    sources[numSources++] = ops;
    return 0;
}

size_t messages_task(uint8_t mode)
{
    const struct messageOps *ops = findSource(mode);

    if (ctx == NULL) {
        if (ops == NULL)
            return 0;

        allocate();
        if (ctx == NULL)
            return 0;
    }

    for (size_t i = 0; i < ARRAY_SIZE(ctx->rxSlots); i++)
        handleRx(&ctx->rxSlots[i], ops, mode);

    handleTx();

    /*
     * No source for the current mode: release the storage as soon as the
     * rtx stage has handed back every descriptor, dropping the content.
     */
    if (ops == NULL) {
        if (!slotsBusy())
            messages_terminate();

        return 0;
    }

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
        if (ctx->entries[i].unread != 0)
            unread++;
    }

    return unread;
}

const struct message *messages_get(size_t idx)
{
    if (idx >= messages_count())
        return NULL;

    return &ctx->entries[ctx->numEntries - idx - 1];
}

size_t messages_findBySequence(uint32_t sequence)
{
    for (size_t i = 0; i < messages_count(); i++) {
        if (ctx->entries[i].sequence == sequence)
            return ctx->numEntries - i - 1;
    }

    return SIZE_MAX;
}

int messages_store(const struct message *msg, uint32_t *sequence)
{
    if ((msg == NULL) || ((msg->body == NULL) && (msg->bodyLen != 0)))
        return -EINVAL;

    if (msg->bodyLen >= MSG_BODY_MAX_LEN)
        return -EMSGSIZE;

    if (ctx == NULL)
        return -ENODEV;

    if (ctx->numEntries == ARRAY_SIZE(ctx->entries))
        removeEntry(0);

    uint16_t offset = poolAlloc(msg->bodyLen);
    if (msg->bodyLen > 0)
        memcpy(&ctx->pool[offset], msg->body, msg->bodyLen);
    ctx->pool[offset + msg->bodyLen] = '\0';

    struct message *entry = &ctx->entries[ctx->numEntries++];
    *entry = *msg;
    entry->body = &ctx->pool[offset];
    entry->sequence = nextSequence++;
    entry->sender[MSG_ADDR_MAX_LEN - 1] = '\0';
    entry->recipient[MSG_ADDR_MAX_LEN - 1] = '\0';

    if ((entry->direction == MSG_DIR_RX) && (entry->unread != 0))
        ctx->newArrivals++;

    if (sequence != NULL)
        *sequence = entry->sequence;

    return 0;
}

int messages_setStatus(uint32_t sequence, enum messageStatus status)
{
    size_t idx = messages_findBySequence(sequence);

    if (idx == SIZE_MAX)
        return -ENOENT;

    ctx->entries[ctx->numEntries - idx - 1].status = status;
    return 0;
}

int messages_markRead(size_t idx, bool read)
{
    if (idx >= messages_count())
        return -ENOENT;

    ctx->entries[ctx->numEntries - idx - 1].unread = read ? 0 : 1;
    return 0;
}

int messages_delete(size_t idx)
{
    if (idx >= messages_count())
        return -ENOENT;

    removeEntry(ctx->numEntries - idx - 1);
    return 0;
}

bool messages_canCompose(uint8_t mode)
{
    return (findSource(mode) != NULL) && (ctx != NULL);
}

int messages_send(const struct message *msg)
{
    if (msg == NULL)
        return -EINVAL;

    if (findSource(msg->mode) == NULL)
        return -ENOENT;

    if (ctx == NULL)
        return -ENODEV;

    struct message entry = *msg;
    entry.direction = MSG_DIR_TX;
    entry.status = MSG_STATUS_SENDING;
    entry.unread = 0;

    int ret = messages_store(&entry, NULL);
    if (ret != 0)
        return ret;

    /* The entry just stored is the newest one. */
    struct message *stored = &ctx->entries[ctx->numEntries - 1];
    ret = transmit(stored);
    if (ret != 0)
        stored->status = MSG_STATUS_FAILED;

    return ret;
}
