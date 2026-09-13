<!--
SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Messaging rework plan

Working document for the rework of PR #492 ("Message registry, inbox, and
M17 SMS mode") into three chained upstream PRs. This file lives only on
`feature/m17-sms-messaging` (the full chain) and is never cherry-picked
into a PR branch. It is the anchor for keeping PRs 2 and 3 consistent with
feedback received on PR 1.

## 1. Feedback received

### 2026-09-12 — Silvano, call (on #492 as pushed 2026-09-08)

Likes the feature. Wants the change split so it can be reviewed in depth:
at least two, probably three chained PRs — (1) message engine / registry
with no UI, (2) the M17 implementation, (3) the UI including notifications.

Code feedback:

- `packet_io` queues are managed inside `m17_sms`, an M17-specific module.
  The queues should be protocol independent: just a buffer. Put the queue
  exchange in a common module (working name: "message dispatcher") that
  hands a packet to the protocol handler, which only then translates it
  into a protocol-specific structure. Until the content is needed, treat
  a message as bytes we don't care about.
- This moves the work off the RTX thread (`m17_sms_task_rtx`) onto the UI
  / main side, since it's just a packet. In Silvano's unpushed APRS/TNC
  rewrite a main-thread dispatcher pulls packet descriptors from rtx one
  at a time, forwards to the serial interface and to the UI; the same
  pattern should serve here (with a separate TNC queue there).
- The message body pool should likewise be protocol independent,
  reusable and shareable.
- As zero-copy as reasonably possible.
- `supported_actions`: is it needed? We would never ship a type that
  doesn't support all actions (APRS wouldn't be released without TX/reply).
- Defer ringtones / RTTTL; use a simple beep. Ringtone sequencer is a
  separate future commit series.

Context: Silvano's APRS work is `origin/aprs-rebase-v2` (shares the
`release-v0.5.0` base and the M17 packet commits). Before the inbox
releases he will finish that branch, drop its UI component, and adopt
the inbox. Therefore: no demo sources, and the inbox never ships with
fewer than two modes in tree.

## 2. Confirmed decisions (Ryan, 2026-09-12)

| # | Decision |
|---|----------|
| D1 | `packet_io.[ch]` and `m17_sms_task_rtx()` are removed. `struct pktDesc` + `rtx_addPacketRx/Tx()` is already the thread-safe RTX boundary (M17: ringbuf for RX descriptors, atomic CAS for TX). |
| D2 | A protocol-agnostic **dispatcher** owns a small pool of `{pktDesc, buffer}` slots, arms idle slots for RX, polls status, hands `(mode, buffer, size)` to the source registered for the current mode, submits TX and polls for completion. |
| D3 | Dispatcher and registry both tick on the **UI thread** (least rework; zero cross-thread state). Silvano's `packet_engine.c` runs on `main_thread` because it also feeds a KISS/TNC over USB. Moving ours later is one call site plus a lock. |
| D4 | Sources receive opaque `(buffer, size)`; the slot buffer is sized to the largest protocol packet. |
| D5 | Zero-copy is aspirational, not at the cost of capacity. RX path: one `memcpy` from the dispatcher RX slot into the shared variable-length body pool. Capacity (messages per target) is preserved. |
| D6 | Single consumer per mode. Callback shape must not preclude fan-out later. |
| D7 | **Registry owns all storage** (entry array + shared body pool). Sources are stateless translators: `process_rx(buf, len)` parses and calls `messages_store(...)`; `format_tx(...)` builds the outgoing packet into the dispatcher's TX slot. `count/get/tick/invoke_action/supported_actions/start_compose` and `enum message_action` are removed. UI calls `messages_delete()` / `messages_mark_read()` directly. TX completion: dispatcher remembers the in-flight entry's sequence and calls `messages_set_status(seq, SENT\|FAILED)`; sources are not involved. Rationale: persistence becomes one serializer over one layout. Protocol-specific per-entry metadata (e.g. APRS message-ID) goes in a small generic field on the shared entry, never a source-owned side table. |
| D8 | Compose availability = "a source is registered for this mode". Reply gated on `entry.mode == current mode` (as the UI does today). |
| D9 | Drop `rtttl.[ch]`, the tone table, `msg_notification_tone`, `NOTIFY_VIBE` / `NOTIFY_TONE_VIBE`. Notification is a single beep via `vp_beep()`, which already follows the accessibility beep level (`vpLevel >= vpBeep`). No new settings fields unless a Notification on/off toggle survives review. |
| D10 | Do not attempt to generalise or replace Silvano's `packet_engine.c`; he has not flagged what he'll do with it. |
| D11 | `hwconfig.h` `CONFIG_MESSAGES` enables go in PR 3. |
| D12 | Three **chained PRs**, each with several small focused commits per repo norms (every commit builds and passes tests; review fixes amended into the originating commit). |
| D13 | Only the PR 1 branch is cut now. PRs 2 and 3 remain commits on the full chain until PR 1 review settles. |
| D14 | Sources receive the completed `struct pktDesc` itself (opaque buffer + `size`/`res`), since opmodes report packet length differently (M17: payload length in `res`; APRS: total length in `size`). The dispatcher never interprets it. |
| D15 | Sources are registered at run time (`message_dispatcher_register()`, from `threads.c` under `CONFIG_M17_SMS`) rather than through a compile-time table, so the dispatcher is unit-testable with fake sources and the `#ifdef` lives in one place. |
| D16 | `CONFIG_MESSAGES` is enabled only where `CONFIG_M17` is (not GDx / MD-9600): an inbox with no source would be an empty menu forever. |
| D17 | The compose screen stays under `CONFIG_M17_SMS` for now (default recipient is the M17 destination, broadcast is "ALL"); generalising it is APRS-adoption work. |
| D18 | The `PLATFORM_LINUX` stderr hook and `scripts/sms_loopback_test.sh` are dropped. The Catch2 loopback in `tests/unit/m17_sms.cpp` covers the round trip; the emulator end-to-end check (below) is a manual procedure. |

## 3. Target architecture

```
 UI thread
 ┌───────────────────────────────────────────────────────────────────┐
 │ ui_*            messages_*() facade         dispatcher            │
 │  compose ──────► messages_send(mode,dst,body)                     │
 │                    ├─ messages_store(entry, body → pool)          │
 │                    └─ ops[mode].format_tx() ──► TX slot ──► rtx_addPacketTx()
 │  list/detail ◄─── messages_get()/count()                          │
 │  delete/read ───► messages_delete()/mark_read()                   │
 │                                                                   │
 │  tick: dispatcher polls slots                                     │
 │    RX DONE ──► ops[mode].process_rx(buf,len) ──► messages_store() │
 │    TX DONE/ERROR ──► messages_set_status(seq, SENT|FAILED)        │
 │    idle RX slot ──► rtx_addPacketRx()                             │
 └───────────────────────────────────────────────────────────────────┘
                              │ struct pktDesc (already thread-safe)
 RTX thread                   ▼
   OpMode_M17 / OpMode_APRS fill / drain descriptors
```

Registry storage: fixed entry array (`CONFIG_MESSAGES_MAX_ENTRIES`) +
variable-length ring body pool (`CONFIG_MESSAGES_POOL_BYTES`), both
per-target in `hwconfig.h`. Entry = `{mode, type, direction, status,
unread, sequence, sender, recipient, body offset/len}`.

Reduced source contract (`core/messages.h`, after the 2026-09-16 review):

```c
struct messageOps {
    int (*processRx)(const struct pktDesc *pkt);   /* parse → messages_store() */
    int (*formatTx)(const struct message *msg, struct pktDesc *pkt);
    const char *name;   /* "M17 SMS" */
    uint8_t mode;       /* enum opmode */
};
```

Inbox API (`core/messages.h`): `messages_init/terminate/task/registerSource`,
`messages_count/countUnread/get/findBySequence`, `messages_store`,
`messages_setStatus`, `messages_markRead`, `messages_delete`,
`messages_canCompose`, `messages_send`. Storage is one heap block allocated by
`messages_task()` when the opmode has a source and freed when it has none,
once the rtx stage has handed every descriptor back. `messages_send()` takes a
`struct message` template (mode, sender, recipient, body) so the registry
needs no access to `state`.

## 4. PR plan and commits (as reworked, 2026-09-12)

Every commit builds for linux, all cm4 targets and cs7000p, and passes the
unit test suite.

### PR 1 — message inbox (no UI, no M17) — off `release-v0.5.0`

1. `rtx: hand back pending packet descriptors when M17 mode is disabled` —
   `OpMode_M17` completes queued/in-flight descriptors with
   `PKT_STATUS_ERROR` / `-ECANCELED` on `disable()` and `enable()`, so a
   buffer owner can release memory once it has left M17.
2. `core: add message inbox` — one module (`core/messages.[ch]`): entries +
   shared pool, run-time source registration (`struct messageOps`), packet
   slots and TX submit/poll, storage heap-allocated per opmode.
   `tests/unit/messages.cpp` with `tests/unit/rtx_packet_stub.*`.
3. `core: run the message inbox from the UI thread` — `threads.c`
   init/task/terminate under `CONFIG_MESSAGES`.

### PR 2 — M17 SMS source — off PR 1 tip

4. `m17: add non-copying accessor for SMS packet text` —
   `sms_packet_text()`; `sms_parse_packet()` becomes a wrapper.
5. `core: add M17 SMS message source` — `process_rx`/`format_tx` over
   `struct m17Packet`, registration under `CONFIG_M17_SMS`.
   `tests/unit/m17_sms.cpp` includes the dispatcher loopback.

### PR 3 — UI and notifications — off PR 2 tip

6. `ui: sync RTX identity immediately after changing M17 callsign` —
   unchanged from #492 (already signed off); could also go upstream on
   its own.
7. `ui: add mail symbol to the symbol fonts`.
8. `ui: add message inbox, detail and compose screens` — one commit: a
   list without a working "New Message" row would be a broken
   intermediate state.
9. `core: beep when a message is received` — `vp_beep(BEEP_NEW_MESSAGE)`.
10. `hwconfig: enable the message inbox and M17 SMS on M17 capable
    targets`.

Removed relative to #492: `core: add packet_io TX/RX queue`,
`core: add notification tone engine and message tone trigger` (rtttl),
`ui: add Settings > Messages submenu`, `test: add SMS loopback TX/RX
integration test` (folded into the M17 source unit test), the `meta/`
emulator scripts and `scripts/sms_loopback_test.sh`.

### Manual end-to-end check (emulator)

Verified 2026-09-12 on the full chain. With the linux build in M17 mode:

1. TX: drive `openrtx_linux` (`SDL_VIDEODRIVER=dummy`) over stdin:
   `ENTER`, `DOWN x4` (Messages), `ENTER`, `ENTER` (New Message), `ENTER`
   (edit body), `2`, `ENTER`, `DOWN` (Send), `ENTER`, wait 4 s, `quit`.
   The list shows `ALL  a` and `/tmp/m17_output.raw` holds the baseband.
2. `sox -r 48000 -e signed-integer -b 16 -c 1 /tmp/m17_output.raw -r 24000
   /tmp/baseband.raw` (the linux `SOURCE_RTX` reads and loops this file).
3. RX: run again, wait 8 s, open Messages: entries `*<callsign> a` appear
   (one per loop of the file), the detail view shows sender > ALL, the
   body, and Reply; the counter keeps the selection pinned while more
   arrive.

## 5. Branch workflow

- `feature/m17-sms-messaging` (local + `fork`): the full chain, always
  buildable per commit. This document lives here.
- Per-PR branches are cut from the chain: PR 1 off `release-v0.5.0`;
  PR 2 off PR 1's tip; PR 3 off PR 2's tip. Only PR 1 is cut for now (D13).
- #492 will be closed; new PRs opened per branch. Never push without an
  explicit OK.

## 6. Review impact log

Record each round of PR feedback here with its effect on the not-yet-open
PRs, so the chain stays consistent with what has already been agreed.

| Date | PR | Feedback | Impact on PR 2 | Impact on PR 3 |
|------|----|----------|----------------|----------------|
| 2026-09-16 | #509 | Silvano: fold the dispatcher into the messages module (`processRx`/`formatTx` as callbacks in the ops struct); use `moduleName_functionName` naming; heap-allocate the large buffers when switching into an opmode with a source and release them on switching out (inbox content dropped; persistence later). | `m17_sms.[h,cpp]`: `struct messageOps`, `bodyLen`, `MSG_PKT_MAX_SIZE` from `core/messages.h`; `threads.c` registers with `messages_registerSource()`; tests activate the inbox (`messages_registerSource()` + `messages_task(OPMODE_M17)`) before storing, and take the arrival count from the `messages_task()` return. | `ui.c`/`ui_messages.c`/`ui_main.c`: `messages_countUnread`, `messages_findBySequence`, `messages_markRead`, `messages_canCompose`, `bodyLen`; the beep reads `messages_task()`'s return; the unread badge is 0 outside M17 (storage freed); `SpanishStrings.h` new strings go after `metaText` (table reordered on master). |
| 2026-09-17 | #509 | Self-review before re-request: agent-added sign-offs stripped (AGENTS.md); shutdown use-after-free (UI thread freed storage while the rtx thread cancelled descriptors inside it) fixed by making `messages_terminate()` keep storage the rtx stage still references; `processRx` receive-length contract documented (`res` = bytes received, `size` = buffer) and the stub/tests aligned; eviction made strictly oldest-first across a pool wrap; `OpMode_M17` cancels pending descriptors on `enable()` as well as `disable()`. | None beyond the above: `m17_sms.cpp` already used `res`. | The eviction test is written against `CONFIG_MESSAGES_*` so it passes with the linux hwconfig sizes (64 / 6400) enabled by commit 10. |
