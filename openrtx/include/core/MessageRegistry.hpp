/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MESSAGE_REGISTRY_HPP
#define MESSAGE_REGISTRY_HPP

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include "core/messages.h"
#include <cstddef>
#include "hwconfig.h"

/**
 * Maximum number of concurrently registered message sources.
 */
static constexpr size_t MAX_SOURCES = 4;

/**
 * Maximum number of entries held in a single snapshot across all sources.
 * Entries beyond this cap are silently dropped (oldest first per source order).
 * Configured per target in hwconfig.h via CONFIG_MSG_SNAPSHOT_SIZE.
 */
static constexpr size_t MAX_MESSAGES_SNAPSHOT = CONFIG_MSG_SNAPSHOT_SIZE;

/**
 * \brief Protocol-agnostic message registry.
 *
 * The registry is a multiplexer — it does **not** own message storage.
 * Each registered source provides a `count()` / `get(i)` callback pair;
 * the registry holds borrowed references.
 *
 * A sorted snapshot (newest-first by `timestamp`) is rebuilt on every call
 * to `tick()`. Pointers in the snapshot are valid until the next `tick()`.
 *
 * All methods must be called from the UI/state thread.  No internal locking
 * is performed.
 */
class MessageRegistry
{
public:
    /**
     * \brief Initialise the registry; clears all slots and the snapshot.
     */
    void init();

    /**
     * \brief Release all registrations and clear the snapshot.
     */
    void terminate();

    /**
     * \brief Register a new message source.
     *
     * @param vtable: pointer to the source's vtable; must remain valid for
     *                the lifetime of the registration.
     * @param ctx: opaque context pointer forwarded to vtable callbacks.
     * @return non-negative handle on success, or -1 if no free slot exists.
     */
    int registerSource(const message_type_vtable_t *vtable, void *ctx);

    /**
     * \brief Unregister a previously registered source.
     *
     * Entries from this source are removed from the snapshot on the next
     * `tick()`.
     *
     * @param handle: handle returned by registerSource().
     */
    void unregisterSource(int handle);

    /**
     * \brief Rebuild the sorted snapshot from all registered sources.
     *
     * Uses a stable insertion sort (O(n^2), n <= MAX_MESSAGES_SNAPSHOT) to
     * order entries by `timestamp` descending (newest first).
     */
    void tick();

    /**
     * \brief Return the number of entries in the current snapshot.
     *
     * @return entry count.
     */
    size_t count() const;

    /**
     * \brief Return the number of unread entries in the current snapshot.
     *
     * @return count of entries with `unread == true`.
     */
    size_t countUnread() const;

    /**
     * \brief Return a pointer to the \p idx-th snapshot entry.
     *
     * Valid until the next `tick()`.
     *
     * @param idx: zero-based index.
     * @return borrowed pointer, or nullptr if out of range.
     */
    message_header_t *get(size_t idx) const;

    /**
     * \brief Invoke an action on the entry at snapshot index \p idx.
     *
     * @param idx: zero-based snapshot index.
     * @param action: action to invoke.
     * @return 0 on success, -ENOENT if out of range, -ENOTSUP if the action
     *         is not supported, or another negative errno on failure.
     */
    int invokeAction(size_t idx, message_action_t action);

    /**
     * \brief Return the number of sources whose vtable has `start_compose`.
     *
     * @return count of compose-capable sources.
     */
    size_t composeSourceCount() const;

    /**
     * \brief Return the display name of the \p i-th compose-capable source.
     *
     * @param i: zero-based compose-source index.
     * @return name string, or nullptr if out of range.
     */
    const char *composeSourceName(size_t i) const;

    /**
     * \brief Invoke `start_compose` on the \p i-th compose-capable source.
     *
     * @param i: zero-based compose-source index.
     * @return 0 on success, -ENOENT if out of range.
     */
    int startCompose(size_t i);

private:
    /**
     * \brief Per-source registration slot.
     */
    struct SourceSlot
    {
        const message_type_vtable_t *vtable;
        void *                       ctx;
        bool                         inUse;
        uint16_t                     typeId; /**< Assigned type id (slot index +
                                               1). */
    };

    SourceSlot        sources[MAX_SOURCES];
    message_header_t *snapshot[MAX_MESSAGES_SNAPSHOT];
    size_t            snapshotLen;
};

#endif /* MESSAGE_REGISTRY_HPP */
