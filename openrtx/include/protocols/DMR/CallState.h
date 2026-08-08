/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_CALLSTATE_H
#define DMR_CALLSTATE_H

#include <stdint.h>
#include <stdbool.h>
#include "protocols/DMR/Datatypes.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * State of a received DMR call, tracked per logical slot from the stream of
 * bursts a modem delivers. This layer owns the mechanism - confirming a link
 * control, telling a live call from a terminated one, gating the voice - and
 * leaves the policy to its caller: whether a confirmed call is accepted is
 * decided above, as is everything involving audio hardware or a log line.
 *
 * Time enters exclusively through the caller's clock parameter, a
 * millisecond tick; nothing here reads a platform clock, which is what lets
 * every rule in this file run on the host under test.
 */

/**
 * The link control has to be seen this many times before it is believed: the
 * modem hands back the link control of the previous transmission until it
 * has decoded the current one, so the same content has to show up in more
 * than one burst before it can be trusted.
 */
#define DMR_LC_CONFIRM_COUNT 2

/**
 * Window within which two bursts carrying decodable voice must fall for the
 * voice to be believed, in milliseconds. Real voice bursts arrive every
 * 60ms, so the second one opens the gate; the noise bursts which rarely
 * clear the per-frame check land much further apart than this and never
 * pair up.
 */
#define DMR_VOICE_CONFIRM_WINDOW 250

/**
 * How long after a terminator burst the voice has to stay gone before the
 * call is truly considered over, in milliseconds. A real terminator is the
 * last burst of its transmission, but a burst's type can also be misread as
 * one in the middle of a call, so a terminator is only believed once the
 * silence it promises actually follows; the next voice burst of a live call
 * arrives within 60ms and cancels it.
 */
#define DMR_TERMINATOR_CONFIRM 150

/**
 * How long the receive state is held after the last good burst before the
 * call is considered over, in milliseconds. Voice bursts on the received
 * slot are 60ms apart, so this rides out a few corrupted bursts without
 * keeping the call up after the carrier is gone.
 */
#define DMR_RX_HOLD_TIMEOUT 500

/** Flags returned by dmr_callState_feedBurst, combined by OR. */
enum dmrCallStateFlags {
    DMR_CALLSTATE_NEW_CALL = 0x01, ///< A call has been confirmed; the caller
                                   ///  decides whether to accept it.
};

/** Why dmr_callState_update considered the call over. */
enum dmrCallEnd {
    DMR_CALL_END_NONE = 0,   ///< The call is still up, or there is none.
    DMR_CALL_END_TERMINATOR, ///< A terminator's promised silence held.
    DMR_CALL_END_TIMEOUT,    ///< The carrier went away without one.
};

/**
 * Everything tracked for one logical slot. The candidate holds the
 * identities of a newly confirmed call between the DMR_CALLSTATE_NEW_CALL
 * flag being returned and the caller's accept or reject decision; the call
 * is what the caller accepted, and what the user interface shows.
 */
struct dmrCallState {
    struct dmrCall call;      ///< The call being received, when call.valid.
    struct dmrCall candidate; ///< A confirmed call awaiting the decision.
    uint8_t prevLc[DMR_LC_LENGTH]; ///< Last link control seen, for the dedupe.
    uint8_t lcMatches;             ///< Times prevLc has been seen in a row.
    bool lcDecided;                ///< The confirmed link control was acted on.
    uint8_t voiceStreak;     ///< Bursts in a row carrying decodable voice.
    int64_t lastVoiceTick;   ///< Tick of the last decodable voice burst.
    int64_t termPendingTick; ///< Tick of an unconfirmed terminator burst.
    int64_t lastBurstTick;   ///< Tick of the last burst event seen.
};

/**
 * Reset a slot's state to no call and nothing tracked.
 *
 * @param cs: state to initialise.
 */
void dmr_callState_init(struct dmrCallState *cs);

/**
 * Feed one received burst into the state machine. Every burst event belongs
 * here, including corrupted and unclassified ones: liveness and the
 * terminator rules feed on events which fail their checks.
 *
 * @param cs: state of the slot the burst arrived on.
 * @param burst: the burst, as the modem delivered it.
 * @param now: current time, in milliseconds.
 * @return zero or more of enum dmrCallStateFlags.
 */
uint8_t dmr_callState_feedBurst(struct dmrCallState *cs,
                                const struct dmrBurst *burst,
                                const int64_t now);

/**
 * Accept the candidate call: its identities become the tracked call and the
 * caller starts playback.
 *
 * @param cs: state holding a candidate to accept.
 */
void dmr_callState_accept(struct dmrCallState *cs);

/**
 * Turn the candidate call away. A call already in progress is dropped with
 * it: playback follows the call state, so leaving it up would let the
 * rejected call be heard through the tail of the one it interrupted.
 *
 * @param cs: state holding a candidate to reject.
 */
void dmr_callState_reject(struct dmrCallState *cs);

/**
 * Whether the voice gate is open: enough bursts in a row carried decodable
 * voice for this burst's frames to be believed and played. A single passing
 * burst is not: the noise on the unused slot clears the per-frame check just
 * often enough that a rejected call would blip through.
 *
 * @param cs: state of the slot being received.
 * @return true if the current burst's voice frames may be played.
 */
bool dmr_callState_voiceGateOpen(const struct dmrCallState *cs);

/**
 * Periodic update: refreshes whether voice is still arriving and decides
 * whether the call is over, either behind a terminator whose silence held or
 * by the carrier going away. When it reports an end, everything tracked has
 * already been dropped, so the next transmission is treated as a fresh call
 * from its first burst; the identities stay behind as "last heard".
 *
 * @param cs: state of the slot being received.
 * @param now: current time, in milliseconds.
 * @return why the call ended, or DMR_CALL_END_NONE.
 */
enum dmrCallEnd dmr_callState_update(struct dmrCallState *cs,
                                     const int64_t now);

/**
 * Restart the burst tracking after the receive path was away, for a
 * transmission of our own: the link control tracking, the voice gate and
 * the observed colour code start over, and the liveness clock restarts now
 * so an old call does not time out on the spot.
 *
 * @param cs: state of the slot being received.
 * @param now: current time, in milliseconds.
 */
void dmr_callState_restartTracking(struct dmrCallState *cs, const int64_t now);

/**
 * Drop the link control tracking alone, for a configuration change which
 * restarts every call in progress at the modem: the next transmission is
 * confirmed afresh, while the voice gate and liveness ride through.
 *
 * @param cs: state of the slot being received.
 */
void dmr_callState_resetLcTracking(struct dmrCallState *cs);

#ifdef __cplusplus
}
#endif

#endif /* DMR_CALLSTATE_H */
