/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * \file messages.h
 * \brief Generic protocol-agnostic message inbox API.
 *
 * ## Overview
 *
 * This module provides a unified inbox that aggregates received (and outgoing)
 * messages from one or more protocol sources (e.g. M17 SMS, APRS).
 *
 * ## How to register a source
 *
 * 1. Define an entry struct whose **first field** is `message_header_t`:
 *    \code{.c}
 *    typedef struct {
 *        message_header_t hdr;   // MUST be first
 *        char             body[128];
 *    } my_entry_t;
 *    \endcode
 *
 * 2. Fill in a `message_type_vtable_t` with at least `count` and `get`.
 *
 * 3. Call `MessageRegistry::registerSource()` at startup (C++ modules) or
 *    expose a C wrapper that does so.
 *
 * ## Threading rules
 *
 * All `messages_*` C facade calls MUST be made from the **UI/state thread**.
 * Protocol producers running on the RTX thread MUST stage data via the
 * existing `rtxStatus_t` handoff pattern and must not call the registry
 * directly.  The registry takes no internal lock.
 *
 * ## Header-embed requirement
 *
 * `message_header_t` MUST be the first field of every per-protocol entry
 * struct.  The registry casts `void *` pointers to `message_header_t *`
 * without an offset.
 *
 * ## Snapshot lifetime
 *
 * Pointers returned by `messages_get()` are valid only until the next call
 * to `messages_tick()`.  UI code must not cache them across ticks.
 *
 * ## Persistence
 *
 * Out of scope.  The registry is purely in-memory; all state is cleared on
 * reboot.
 *
 * ## Eviction
 *
 * Source-controlled.  When a source's ring buffer evicts an entry, unread
 * state is lost with it.  This is documented and accepted behaviour.
 *
 * ## Migration / Integration Notes
 *
 * Protocol modules integrating with this inbox should follow these steps:
 *
 * 1. **Define a protocol entry struct** whose first field is
 *    `message_header_t`.  The registry casts stored pointers to
 *    `message_header_t *` without an offset, so this is mandatory.
 *
 * 2. **Implement a vtable** (`message_type_vtable_t`).  At minimum,
 *    `count` and `get` must be non-NULL.  Provide `supported_actions` and
 *    `invoke_action` to support mark-read, delete, and reply.  Provide
 *    `start_compose` (and set `name`) to appear in the "New" picker.
 *
 * 3. **Wire the source at compile time** by adding a `SourceEntry` to the
 *    static array in `messages.cpp`, guarded by the appropriate
 *    `#ifdef CONFIG_*` flag.  No runtime registration call is needed.
 *
 * 4. **Populate headers** when a message arrives.  Copy the callsign or
 *    address into `sender` (max 15 chars + NUL) and set `body` to point
 *    into source-owned storage with the payload length in `body_len`.
 *    Set `unread = true` for incoming
 *    messages.  Assign a monotonically increasing `timestamp` (Unix epoch
 *    seconds or a local counter).
 *
 * 5. **Do not call the registry from the RTX thread.**  Stage incoming
 *    data via `rtxStatus_t` and process it in the UI tick.
 *
 * ### Known risks / design trade-offs
 *
 * - **Embed-header coupling**: The "first field" constraint means protocol
 *   entry structs are not ABI-independent.  All consumers must be rebuilt
 *   together.
 * - **Snapshot-index API**: UI row indices are valid only until the next
 *   `messages_tick()`.  Protocol code must not cache snapshot pointers.
 * - **Unread bit lives in source storage**: If a source evicts an entry
 *   before the user reads it, the unread count silently drops.  Accepted
 *   for the MVP; persistent storage is out of scope.
 */

#ifndef MESSAGES_H
#define MESSAGES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "core/graphics.h"
#include "core/input.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \brief Direction of a message (received or transmitted).
 */
typedef enum
{
    MSG_DIR_RX = 0, /**< Incoming message. */
    MSG_DIR_TX = 1, /**< Outgoing message. */
} message_direction_t;

/**
 * \brief Lifecycle status of a message.
 */
typedef enum
{
    MSG_STATUS_RECEIVED = 0, /**< Successfully received. */
    MSG_STATUS_SENDING,      /**< TX in progress. */
    MSG_STATUS_SENT,         /**< TX acknowledged / completed. */
    MSG_STATUS_FAILED,       /**< TX failed. */
    MSG_STATUS_READ,         /**< RX, already read by the user. */
} message_status_t;

/**
 * \brief Bitmap of actions that may be invoked on a message entry.
 *
 * A source reports which actions it supports via
 * `vtable->supported_actions()`. The UI must check this bitmap before
 * offering an action affordance.
 */
typedef enum
{
    MSG_ACTION_VIEW        = 1u << 0, /**< Open detail view. */
    MSG_ACTION_DELETE      = 1u << 1, /**< Remove the entry. */
    MSG_ACTION_REPLY       = 1u << 2, /**< Launch compose pre-filled w/ sender.*/
    MSG_ACTION_MARK_READ   = 1u << 3, /**< Clear the unread flag. */
    MSG_ACTION_MARK_UNREAD = 1u << 4, /**< Set the unread flag. */
} message_action_t;

/**
 * \brief Common header embedded at the start of every per-protocol entry.
 *
 * Protocol-specific entry structs MUST begin with this field.  The registry
 * accesses all entries through a pointer to this header.
 */
typedef struct
{
    uint16_t           type;      /**< Assigned by registry at registration. */
    uint64_t           timestamp; /**< Monotonic counter for sorting. */
    message_direction_t direction; /**< RX or TX. */
    message_status_t   status;   /**< Lifecycle status. */
    bool               unread;   /**< True when not yet read by the user. */
    char               sender[16];    /**< Originating callsign / address. */
    char               recipient[16]; /**< Destination callsign / address. */
    const void        *body;        /**< Source-owned body pointer; valid
                                        until entry evicted. */
    size_t             body_len;    /**< Byte length of body content. */
} message_header_t;

/**
 * \brief Vtable linking a message source to the registry.
 *
 * All function pointers except `count` and `get` are optional; set to NULL
 * if the source does not support that capability.
 */
typedef struct
{
    /**
     * Display name shown in the "New" picker.  Short, e.g. "M17 SMS".
     * Required when `start_compose` is non-NULL.
     */
    const char *name;

    /**
     * \brief Return the number of entries currently held by this source.
     *
     * @param src_ctx: opaque context pointer supplied at registration.
     * @return current entry count.
     */
    size_t (*count)(void *src_ctx);

    /**
     * \brief Return a pointer to entry \p i.
     *
     * The pointer is borrowed; the registry does not copy it.  The caller
     * (registry) guarantees it will not be accessed after the source has
     * evicted the entry.
     *
     * @param src_ctx: opaque context pointer supplied at registration.
     * @param i: zero-based entry index, 0 <= i < count().
     * @return pointer to the entry's embedded `message_header_t`.
     */
    message_header_t *(*get)(void *src_ctx, size_t i);

    /**
     * \brief Render a single list row for this entry type.
     *
     * When NULL the generic screen renders sender + body[0..N-1] as text.
     * Implement for non-text sources (binary payloads, maps, etc.).
     *
     * @param entry:      borrowed pointer to the entry header.
     * @param pos:        top-left pixel position of the row.
     * @param selected:   true when this row is highlighted.
     * @param text_color: color to use for text rendering.
     */
    void (*render_list_row)(const message_header_t *entry, point_t pos,
                            bool selected, color_t text_color);

    /**
     * \brief Render the detail view for this entry type.
     *
     * When non-NULL fully owns the body area of the detail screen.
     * When NULL the generic screen renders body as scrollable text.
     *
     * @param entry: borrowed pointer to the entry header (valid until next
     *               tick).
     */
    void (*render_detail)(const message_header_t *entry);

    /**
     * \brief Handle a key event while the detail view is open.
     *
     * @param entry: borrowed pointer to the entry header.
     * @param msg: keyboard event.
     * @return true if the event was consumed; false to allow generic handling.
     */
    bool (*handle_detail_input)(message_header_t *entry, kbd_msg_t msg);

    /**
     * \brief Return a bitmap of actions supported for this entry.
     *
     * @param entry: borrowed pointer to the entry header.
     * @return bitwise-OR of `message_action_t` values.
     */
    uint32_t (*supported_actions)(const message_header_t *entry);

    /**
     * \brief Invoke an action on this entry.
     *
     * @param entry: borrowed pointer to the entry header.
     * @param action: one of the `message_action_t` values.
     * @return 0 on success, negative errno on failure.
     */
    int (*invoke_action)(message_header_t *entry, message_action_t action);

    /**
     * \brief Launch the protocol's compose UI (no recipient pre-fill).
     *
     * If non-NULL, this source appears in the list view's "New" picker.
     * Reply (recipient pre-filled from an existing entry's sender) is
     * dispatched via MSG_ACTION_REPLY in invoke_action instead.
     *
     * @param src_ctx: opaque context pointer supplied at registration.
     */
    void (*start_compose)(void *src_ctx);

    /**
     * \brief Optional notification called before an entry is evicted.
     *
     * Allows the source to update internal bookkeeping when the registry
     * would need to drop a snapshot reference.
     *
     * @param entry: pointer to the entry being evicted.
     */
    void (*on_evict)(message_header_t *entry);

    /**
     * \brief Radio operating mode this source serves.
     *
     * Holds one of the `enum opmode` values from rtx/rtx.h stored as a
     * plain uint8_t to avoid a layering dependency on that header.
     * Use OPMODE_NONE (0) for sources that have no associated mode (e.g.
     * demo stubs); such sources are never offered for compose or reply.
     */
    uint8_t mode_id;
} message_type_vtable_t;

/* ---------------------------------------------------------------------- */
/* C facade — all calls must be made from the UI/state thread.             */
/* ---------------------------------------------------------------------- */

/**
 * \brief Initialise the message registry singleton.
 *
 * Called once from state_init().
 */
void messages_init(void);

/**
 * \brief Terminate the message registry and release any resources.
 *
 * Called once from state_terminate().
 */
void messages_terminate(void);

/**
 * \brief Recompute the sorted inbox snapshot.
 *
 * Must be called each UI cycle (typically from the UI thread loop).
 * After this call, pointers from the previous snapshot are invalid.
 */
void messages_tick(void);

/**
 * \brief Return the number of entries in the current snapshot.
 *
 * @return number of entries, sorted newest-first.
 */
size_t messages_count(void);

/**
 * \brief Return the number of unread entries in the current snapshot.
 *
 * @return count of entries with `unread == true`.
 */
size_t messages_count_unread(void);

/**
 * \brief Return a pointer to the \p idx-th entry in the current snapshot.
 *
 * The returned pointer is valid only until the next call to messages_tick().
 * UI code must not cache it across ticks.
 *
 * @param idx: zero-based index, 0 <= idx < messages_count().
 * @return borrowed pointer to the entry header, or NULL if out of range.
 */
message_header_t *messages_get(size_t idx);

/**
 * \brief Invoke an action on the entry at snapshot index \p idx.
 *
 * Dispatches through the entry's source vtable.
 *
 * @param idx: zero-based snapshot index.
 * @param action: action to invoke.
 * @return 0 on success, -ENOENT if idx is out of range, -ENOTSUP if the
 *         action is not supported, or another negative errno on failure.
 */
int messages_invoke_action(size_t idx, message_action_t action);

/**
 * \brief Return true if a compose-capable source exists for \p mode.
 *
 * A source qualifies when its vtable has a non-NULL `start_compose` pointer
 * and its `mode_id` equals \p mode.  OPMODE_NONE (0) never matches.
 *
 * @param mode: one of the `enum opmode` values (stored as uint8_t).
 * @return true if compose is available for that mode.
 */
bool messages_can_compose(uint8_t mode);

/**
 * \brief Invoke `start_compose` on the source registered for \p mode.
 *
 * @param mode: one of the `enum opmode` values (stored as uint8_t).
 * @return 0 on success, -ENOENT if no matching source found.
 */
int messages_start_compose(uint8_t mode);

/**
 * \brief Return the mode_id of the source that produced snapshot entry \p idx.
 *
 * Use this to gate Reply: only offer it when the returned value matches the
 * current operating mode.
 *
 * @param idx: zero-based snapshot index.
 * @return mode_id of the owning source, or 0 if idx is out of range.
 */
uint8_t messages_source_mode(size_t idx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MESSAGES_H */
