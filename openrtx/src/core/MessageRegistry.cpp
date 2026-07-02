/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/MessageRegistry.hpp"
#include <utility>

/* True when @p source has a non-NULL ops table providing callback @p action. */
#define IS_ACTION_DEFINED(source, action) \
    (((source).ops != nullptr) && ((source).ops->action != nullptr))

MessageRegistry::MessageRegistry(const MessageSource *srcs, size_t count) :
    sources(srcs), sourcesLen(count), dirty(true)
{
}

void MessageRegistry::markDirty()
{
    dirty = true;
}

bool MessageRegistry::tick()
{
    /* Give each source a chance to drain the packet_io RX queue and
     * advance TX completion status before we rebuild the snapshot. */
    bool any_changed = false;
    for (size_t s = 0; s < sourcesLen; s++) {
        if (!IS_ACTION_DEFINED(sources[s], tick))
            continue;
        any_changed |= sources[s].ops->tick(sources[s].ctx);
    }

    /* Skip rebuild if nothing has changed since the last tick. */
    if (!dirty && !any_changed)
        return false;

    dirty = false;

    /* Build into the inactive list to avoid stale pointers during rebuild. */
    next->numEntries = 0;

    /* Collect all entries from all sources into the next list. */
    for (size_t s = 0;
         s < sourcesLen && next->numEntries < MAX_MESSAGES_SNAPSHOT; s++) {
        if (!IS_ACTION_DEFINED(sources[s], count)
            || !IS_ACTION_DEFINED(sources[s], get))
            continue;
        size_t n = sources[s].ops->count(sources[s].ctx);
        for (size_t i = 0; i < n && next->numEntries < MAX_MESSAGES_SNAPSHOT;
             i++) {
            struct message_header *hdr = sources[s].ops->get(sources[s].ctx, i);
            if (hdr == nullptr)
                continue;

            next->entries[next->numEntries].hdr = hdr;
            next->entries[next->numEntries].source_idx =
                static_cast<uint8_t>(s);
            next->numEntries++;
        }
    }

    /*
     * Stable insertion sort by sequence descending (newest first).
     * O(n^2) is acceptable because n <= MAX_MESSAGES_SNAPSHOT.
     */
    for (size_t i = 1; i < next->numEntries; i++) {
        SnapshotEntry key = next->entries[i];
        size_t j = i;
        while (j > 0
               && next->entries[j - 1].hdr->sequence < key.hdr->sequence) {
            next->entries[j] = next->entries[j - 1];
            j--;
        }
        next->entries[j] = key;
    }

    /* Swap: make the newly built list the active one. */
    std::swap(curr, next);
    return true;
}

size_t MessageRegistry::count() const
{
    return curr->numEntries;
}

size_t MessageRegistry::countUnread() const
{
    size_t n = 0;
    for (size_t i = 0; i < curr->numEntries; i++) {
        if (curr->entries[i].hdr->unread)
            n++;
    }
    return n;
}

struct message_header *MessageRegistry::get(size_t idx) const
{
    if (idx >= curr->numEntries)
        return nullptr;
    return curr->entries[idx].hdr;
}

size_t MessageRegistry::findBySequence(uint32_t seq) const
{
    for (size_t i = 0; i < curr->numEntries; i++) {
        if (curr->entries[i].hdr->sequence == seq)
            return i;
    }
    return SIZE_MAX;
}

uint32_t MessageRegistry::supportedActions(size_t idx) const
{
    if (idx >= curr->numEntries)
        return 0;

    size_t s = curr->entries[idx].source_idx;
    if (s >= sourcesLen || !IS_ACTION_DEFINED(sources[s], supported_actions))
        return 0;

    return sources[s].ops->supported_actions(curr->entries[idx].hdr);
}

int MessageRegistry::invokeAction(size_t idx, enum message_action action)
{
    if (idx >= curr->numEntries)
        return -ENOENT;

    size_t s = curr->entries[idx].source_idx;
    if (s >= sourcesLen || sources[s].ops == nullptr)
        return -ENOENT;

    struct message_header *hdr = curr->entries[idx].hdr;

    /* Check that the action is supported. */
    if (sources[s].ops->supported_actions != nullptr) {
        uint32_t supported = sources[s].ops->supported_actions(hdr);
        if (!(supported & static_cast<uint32_t>(action)))
            return -ENOTSUP;
    }

    if (sources[s].ops->invoke_action == nullptr)
        return -ENOTSUP;

    int rc = sources[s].ops->invoke_action(hdr, action);
    if (rc == 0)
        markDirty();
    return rc;
}

bool MessageRegistry::canCompose(uint8_t mode) const
{
    if (mode == 0)
        return false;
    for (size_t i = 0; i < sourcesLen; i++) {
        if (IS_ACTION_DEFINED(sources[i], start_compose)
            && sources[i].ops->mode_id == mode)
            return true;
    }
    return false;
}

int MessageRegistry::startCompose(uint8_t mode)
{
    if (mode == 0)
        return -ENOENT;
    for (size_t i = 0; i < sourcesLen; i++) {
        if (IS_ACTION_DEFINED(sources[i], start_compose)
            && sources[i].ops->mode_id == mode) {
            sources[i].ops->start_compose(sources[i].ctx);
            return 0;
        }
    }
    return -ENOENT;
}

uint8_t MessageRegistry::sourceMode(size_t idx) const
{
    if (idx >= curr->numEntries)
        return 0;
    size_t s = curr->entries[idx].source_idx;
    if (s >= sourcesLen || sources[s].ops == nullptr)
        return 0;
    return sources[s].ops->mode_id;
}

int MessageRegistry::send(uint8_t mode, const char *body, size_t body_len,
                          const char *recipient)
{
    if (mode == 0)
        return -ENOENT;
    for (size_t i = 0; i < sourcesLen; i++) {
        if (IS_ACTION_DEFINED(sources[i], send)
            && sources[i].ops->mode_id == mode) {
            int rc = sources[i].ops->send(sources[i].ctx, body, body_len,
                                          recipient);
            if (rc == 0)
                markDirty();
            return rc;
        }
    }
    return -ENOENT;
}
