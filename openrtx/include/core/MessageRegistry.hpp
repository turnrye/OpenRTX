/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MESSAGE_REGISTRY_H
#define MESSAGE_REGISTRY_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include "core/messages.h"
#include <cerrno>
#include <cstddef>

/**
 * @brief A single message source: ops + opaque context.
 *
 * Build an array of these at compile time and pass it to the
 * `MessageRegistry` constructor.  `ops` must be non-NULL and its `count`/
 * `get` members must be non-NULL for every registered source; this is a
 * configuration contract checked defensively (not asserted) by every
 * registry method that dereferences it.
 */
struct MessageSource {
    const struct message_ops *ops;
    void *ctx;
};

/**
 * @brief Protocol-agnostic message registry.
 *
 * The registry is a multiplexer — it does **not** own message storage.
 * Each registered source provides a `count()` / `get(i)` callback pair;
 * the registry holds borrowed references.
 *
 * A sorted snapshot (newest-first by `sequence`) is rebuilt by `tick()`
 * whenever a source reports a change (or the snapshot was explicitly marked
 * dirty).  Pointers in the snapshot are valid until the next `tick()` call
 * that actually rebuilds (i.e. until the next call that returns true).
 *
 * All methods must be called from the UI/state thread.  No internal locking
 * is performed.
 */
class MessageRegistry
{
public:
    /** Maximum snapshot entries across all sources.
     *  Configured per target in hwconfig.h via CONFIG_MSG_SNAPSHOT_SIZE.
     *  Falls back to 1 so the class remains usable (e.g. in unit tests)
     *  when built without a target hwconfig.h setting it. */
    static constexpr size_t MAX_MESSAGES_SNAPSHOT =
#ifdef CONFIG_MSG_SNAPSHOT_SIZE
        CONFIG_MSG_SNAPSHOT_SIZE;
#else
        1;
#endif

    /**
     * @brief Construct the registry over a fixed, compile-time source array.
     *
     * @param sources: pointer to an array of MessageSource structs; must
     *                 remain valid for the lifetime of the registry.
     * @param count:   number of entries in the array.
     */
    MessageRegistry(const MessageSource *sources, size_t count);

    /**
     * @brief Destroy the registry.  The borrowed source array is released; the
     * registry owns no message storage, so nothing else needs cleanup.
     */
    ~MessageRegistry() = default;

    /* Non-copyable: holds borrowed pointers and a large snapshot buffer. */
    MessageRegistry(const MessageRegistry &) = delete;
    MessageRegistry &operator=(const MessageRegistry &) = delete;

    /**
     * @brief Rebuild the sorted snapshot from all registered sources.
     *
     * Invokes each source's tick() callback first (if non-NULL); the
     * bitwise-OR of their return values is combined with the internal dirty
     * flag to decide whether a rebuild is needed this call.  Uses a stable
     * insertion sort (O(n^2), n <= MAX_MESSAGES_SNAPSHOT) to order entries by
     * `sequence` descending (newest first).  A double-buffered approach is
     * used: the new snapshot is built into a separate buffer and only
     * swapped in after sorting completes, so pointers obtained before this
     * call remain valid for the duration of the rebuild.
     *
     * @return true if the snapshot was rebuilt, false if skipped (not dirty
     *         and no source reported a change).
     */
    bool tick();

    /**
     * @brief Return the number of entries in the current snapshot.
     *
     * @return entry count.
     */
    size_t count() const;

    /**
     * @brief Return the number of unread entries in the current snapshot.
     *
     * @return count of entries with `unread == true`.
     */
    size_t countUnread() const;

    /**
     * @brief Return a pointer to the @p idx-th snapshot entry.
     *
     * Valid until the next `tick()` that rebuilds.  The double-buffered
     * approach ensures pointers remain valid during the rebuild phase.
     *
     * @param idx: zero-based index.
     * @return borrowed pointer, or nullptr if out of range.
     */
    struct message_header *get(size_t idx) const;

    /**
     * @brief Find the snapshot index of an entry by its sequence number.
     *
     * Intended for callers that must survive a snapshot rebuild (e.g. the
     * message detail view): pin a sequence number instead of a raw index.
     *
     * @param seq: sequence number to look up.
     * @return snapshot index, or SIZE_MAX if not found.
     */
    size_t findBySequence(uint32_t seq) const;

    /**
     * @brief Return the supported-actions bitmap for snapshot entry @p idx.
     *
     * Dispatches through the source ops's supported_actions() callback.
     *
     * @param idx: zero-based snapshot index.
     * @return action bitmap, or 0 if idx is out of range or the source does
     *         not implement supported_actions().
     */
    uint32_t supportedActions(size_t idx) const;

    /**
     * @brief Invoke an action on the entry at snapshot index @p idx.
     *
     * Marks the snapshot dirty on success so the next tick() rebuilds.
     *
     * @param idx: zero-based snapshot index.
     * @param action: action to invoke.
     * @return 0 on success, -ENOENT if out of range, -ENOTSUP if the action
     *         is not supported, or another negative errno on failure.
     */
    int invokeAction(size_t idx, enum message_action action);

    /**
     * @brief Return true if a compose-capable source exists for @p mode.
     *
     * Matches sources where `ops->start_compose != nullptr` and
     * `ops->mode_id == mode`.  mode == 0 (OPMODE_NONE) never matches.
     *
     * @param mode: operating mode (one of enum opmode, stored as uint8_t).
     * @return true if at least one matching source is registered.
     */
    bool canCompose(uint8_t mode) const;

    /**
     * @brief Invoke `start_compose` on the source registered for @p mode.
     *
     * @param mode: operating mode (one of enum opmode, stored as uint8_t).
     * @return 0 on success, -ENOENT if no matching source found.
     */
    int startCompose(uint8_t mode);

    /**
     * @brief Return the mode_id of the source that produced snapshot entry @p idx.
     *
     * @param idx: zero-based snapshot index.
     * @return mode_id of the owning source, or 0 if idx is out of range.
     */
    uint8_t sourceMode(size_t idx) const;

    /**
     * @brief Enqueue an outgoing message via the source registered for @p mode.
     *
     * Marks the snapshot dirty on success so the next tick() rebuilds.
     *
     * @param mode:      operating mode identifying the target source.
     * @param body:      NUL-terminated message text.
     * @param body_len:  Length of body not counting NUL.
     * @param recipient: Destination callsign.
     * @return 0 on success, -ENOENT if no matching source, or the source
     *         send() return value.
     */
    int send(uint8_t mode, const char *body, size_t body_len,
             const char *recipient);

    /**
     * @brief Mark the snapshot as dirty, forcing a rebuild on next tick().
     *
     * Call this from a source when it adds or removes entries so the UI
     * can skip an unnecessary rebuild on ticks where nothing changed.
     */
    void markDirty();

private:
    /** One resolved snapshot slot: the borrowed header pointer plus which
     *  source it came from (needed to route invokeAction()/sourceMode()
     *  back to the owning source). */
    struct SnapshotEntry {
        struct message_header *hdr;
        uint8_t source_idx;
    };

    /** A full snapshot: resolved entries plus their count. */
    struct SnapshotList {
        SnapshotEntry entries[MAX_MESSAGES_SNAPSHOT] = {};
        size_t numEntries = 0;
    };

    const MessageSource *sources = nullptr;
    size_t sourcesLen = 0;

    /* Double-buffered snapshot: tick() rebuilds into *next and then swaps
     * the two pointers, so *curr always holds the list visible to callers. */
    SnapshotList lists[2];
    SnapshotList *curr = &lists[0];
    SnapshotList *next = &lists[1];

    bool dirty = false;
};

#endif /* MESSAGE_REGISTRY_H */
