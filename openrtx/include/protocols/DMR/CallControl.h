/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_CALLCONTROL_H
#define DMR_CALLCONTROL_H

#include "protocols/DMR/LinkControl.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Following a received DMR voice call, independently of the baseband that
 * receives it: a call starts from the link control of a voice header, or of a
 * superframe's embedded signalling when it is joined part way through; it goes
 * on as long as its bursts keep coming and ends on its own terminator, or when
 * they stop. A receiver keeps one of these per call it follows: one in direct
 * mode, one per timeslot through a repeater.
 */

/**
 * Kinds of received burst, as far as the colour code is concerned.
 */
enum dmrBurstKind {
    DMR_BURST_DATA,       ///< Data or control burst: its slot type has a CC.
    DMR_BURST_VOICE_SYNC, ///< Voice frame A: sync in place of the CC.
    DMR_BURST_VOICE       ///< Voice frames B to F: the EMB has a CC.
};

/**
 * Tell whether a kind of burst carries a colour code to be checked. Voice
 * frame A carries the voice sync where the others carry the embedded
 * signalling with the colour code, so it has none.
 *
 * @param kind: kind of burst.
 * @return true if the burst has a colour code.
 */
static inline bool dmr_burstHasColorCode(const enum dmrBurstKind kind)
{
    return kind != DMR_BURST_VOICE_SYNC;
}

/**
 * State of a received call.
 */
struct dmrRxCall {
    struct dmrVoiceCall call; ///< The call, valid while active.
    uint8_t colorCode;        ///< Colour code it was received with.
    bool active;              ///< A call is in progress.
    long long lastBurst;      ///< When its last burst came, ms.
};

/**
 * What a received link control or terminator did to the call.
 */
enum dmrCallChange {
    DMR_CALL_IGNORED, ///< Not a voice call, or not the call in progress.
    DMR_CALL_SAME,    ///< The call in progress goes on.
    DMR_CALL_STARTED, ///< A new call: a first one, or another station.
    DMR_CALL_ENDED    ///< The call in progress ended.
};

/**
 * Which calls are taken: every one, or only those for this radio, the group
 * calls to its talkgroup and the private calls to its ID.
 */
struct dmrRxFilter {
    bool promiscuous;   ///< Take every call.
    uint32_t ownId;     ///< The radio's DMR ID.
    uint32_t talkgroup; ///< The talkgroup it listens to.
};

/**
 * Forget any call.
 *
 * @param rx: call state.
 */
void dmrCall_reset(struct dmrRxCall *rx);

/**
 * Take the link control of a voice header or of a superframe's embedded
 * signalling.
 *
 * @param rx: call state.
 * @param lc: full link control.
 * @param colorCode: colour code it was received with.
 * @param filter: which calls to take, NULL for every one.
 * @param now: current time, ms.
 * @return DMR_CALL_STARTED for a new call, DMR_CALL_SAME for the one in
 * progress, DMR_CALL_IGNORED if the link control is not a voice call's, or
 * that of a call the filter leaves out.
 */
enum dmrCallChange dmrCall_takeLc(struct dmrRxCall *rx, const uint8_t *lc,
                                  const uint8_t colorCode,
                                  const struct dmrRxFilter *filter,
                                  const long long now);

/**
 * Take the link control of a terminator.
 *
 * @param rx: call state.
 * @param lc: full link control.
 * @return DMR_CALL_ENDED if it ends the call in progress, else
 * DMR_CALL_IGNORED.
 */
enum dmrCallChange dmrCall_takeTerminator(struct dmrRxCall *rx,
                                          const uint8_t *lc);

/**
 * Take a voice burst.
 *
 * @param rx: call state.
 * @param now: current time, ms.
 * @return true if it belongs to a call in progress.
 */
bool dmrCall_takeVoice(struct dmrRxCall *rx, const long long now);

/**
 * Tell whether the call in progress has had no burst for a while, as when a
 * transmission stops without a terminator.
 *
 * @param rx: call state.
 * @param now: current time, ms.
 * @param holdMs: longest time without a burst, ms.
 * @return true if a call is in progress and it is over.
 */
bool dmrCall_expired(const struct dmrRxCall *rx, const long long now,
                     const long long holdMs);

#ifdef __cplusplus
}
#endif

#endif /* DMR_CALLCONTROL_H */
