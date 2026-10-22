/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef INTERFACES_DMR_H
#define INTERFACES_DMR_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The DMR baseband: what the rtx layer's DMR operating mode drives. Each radio
 * with DMR provides it, receiving and transmitting direct mode voice calls
 * through its baseband chip; the DMR protocol layer decides about the calls.
 *
 * The voice frames go through the DMR voice pipeline (core/dmr_audio.h): the
 * baseband queues those it receives for playback and takes those it sends from
 * the microphone, as each burst comes. Starting and stopping the playback and
 * the microphone is left to the operating mode.
 */

/**
 * Information carried by the link control of the last accepted burst, which is
 * what the user interface shows as "last heard".
 */
struct dmrRxState {
    uint32_t srcId;    ///< DMR ID of the station being received.
    uint32_t dstId;    ///< Talkgroup or destination the call is sent to.
    uint8_t colorCode; ///< Color code the burst was received with.
    bool groupCall;    ///< True for a group call, false for a private one.
    bool encrypted;    ///< The call's voice is encrypted.
    bool valid;        ///< True once the link control has been confirmed.
};

/**
 * How the channel is reached: directly, both stations on one frequency, or
 * through a repeater, which receives on one frequency and transmits on the
 * other.
 */
enum dmrAccess {
    DMR_ACCESS_DIRECT,  ///< Direct mode.
    DMR_ACCESS_REPEATER ///< Through a repeater.
};

/**
 * What the baseband needs to know about the channel and the radio.
 */
struct dmrConfig {
    enum dmrAccess access; ///< Direct mode or through a repeater.
    uint8_t colorCode;     ///< Color code to accept and to send, 0 to 15.
    uint8_t timeslot;      ///< Timeslot of the channel, 1 or 2.
    uint32_t ownId;        ///< The radio's own DMR ID, sent as the source.
    bool rxPromiscuous;    ///< Accept calls to any destination.
};

/**
 * Bring the baseband up for DMR reception and start handling its interrupts.
 *
 * @param config: channel and radio configuration.
 */
void dmr_init(const struct dmrConfig *config);

/**
 * Stop the DMR reception.
 */
void dmr_terminate();

/**
 * Apply a new configuration to the running baseband.
 *
 * @param config: channel and radio configuration.
 */
void dmr_configure(const struct dmrConfig *config);

/**
 * Periodic update step, has to be called from a thread as it accesses the
 * baseband over SPI.
 *
 * @param rx: destination for the current receive state.
 */
void dmr_task(struct dmrRxState *rx);

/**
 * Key a group or private call from the radio's own DMR ID. The transmission
 * runs until dmr_stopTx is called; this only sets it up.
 *
 * @param groupCall: true for a group call, false for a private one.
 * @param dstId: destination address, a talkgroup for a group call.
 */
void dmr_startTx(const bool groupCall, const uint32_t dstId);

/**
 * Ask the current transmission to end. The terminator is sent at the next
 * superframe boundary, after which dmr_txActive returns false.
 */
void dmr_stopTx();

/**
 * Periodic transmit-side housekeeping, called from a thread while a
 * transmission is in progress, in place of dmr_task(). It keeps the interrupt
 * watchdog fed, which dmr_task() normally does.
 */
void dmr_txTask();

/**
 * Check whether a transmission is still in progress.
 *
 * @return true until the terminator has been sent and the chip handed back to
 * reception.
 */
bool dmr_txActive();

#ifdef __cplusplus
}
#endif

#endif /* INTERFACES_DMR_H */
