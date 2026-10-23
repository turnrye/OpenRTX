/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/delays.h"
#include "core/dmr_debug.h"
#include "peripherals/gpio.h"
#include "hwconfig.h"
#include "dmrIrq.h"
#include "interfaces/dmr.h"
#include "core/dmr_audio.h"
#include "interfaces/dmr_codec.h"
#include "dmrPayload.h"
#include "protocols/DMR/LinkControl.h"
#include "protocols/DMR/VoiceFrame.h"
#include "protocols/DMR/CallControl.h"
#include "protocols/DMR/VoiceSequence.h"
#include "protocols/DMR/RepeaterSlot.h"
#include "core/threads.h"
#include <pthread.h>
#include <string.h>

#if defined(CONFIG_DMR)

/*
 * DMR runs from a thread of its own, woken by the chip's interrupt lines,
 * receiving and transmitting the way each radio's stock firmware does.
 */

/*
 * The voice payload of a burst: three 9-byte AMBE frames the chip hands over
 * on its second SPI bus, reached through the board's payload bus driver.
 */
#define VP_LENGTH DMR_PAYLOAD_LENGTH

/* Interval between two interrupt activity reports, in milliseconds */
#define IRQ_REPORT_INTERVAL 1000

/*
 * How long the receive state is held after the last good burst before the
 * call is considered over, in milliseconds. Voice bursts on the received
 * slot are 60ms apart, so this rides out a few corrupted bursts without
 * keeping the call on screen after the carrier is gone.
 */
#define RX_HOLD_TIMEOUT 500

static struct dmrRxState rxState;
static struct dmrConfig config; // Channel and radio configuration
static uint8_t configuredCc;    // Colour code of the configuration
static uint32_t reportedTs;
static uint32_t reportedSys;
static uint32_t reportedTx;
static long long reportTick;

/*
 * Payload last handed to playback. The chip keeps a burst's payload until the
 * next burst replaces it and may raise more than one event per burst, so the
 * same payload can be read several times over.
 */
static uint8_t lastPayload[VP_LENGTH];
static bool lastPayloadValid;

/*
 * Chip interrupt flags (register 0x82) and burst status (register 0x51) seen
 * since the last activity report, and which interrupt lines were seen low.
 * They tell a chip that detects nothing apart from interrupts that never
 * reach the processor.
 */
static uint8_t seenFlags;
static uint8_t seenStatus;
static uint8_t seenLinesLow;

static volatile bool txRunning; // A transmission is in progress
static volatile bool txMarker;  // Test pattern in place of the voice
static uint8_t txMarkerSeq;     // Sequence number of the next pattern

static void nextVoicePayload(uint8_t *payload);
static void setupChannel();
static void reportAccess();
static volatile uint8_t txSequence;   // Voice frame sequence, 0..5
static volatile bool txStopRequested; // Unkey asked, waiting for a boundary
static long long txStopTick;          // When the unkey was asked
static volatile bool rxRestoreNeeded; // Receiver bring-up owed after a TX
static uint8_t txLc[DMR_LC_LENGTH];   // Link control sent in the bursts
static uint32_t txReportedTs;         // Timeslot count at the last tx report
static uint32_t txReportedTx;         // Transmit count at the last tx report
static long long txReportTick;        // Tick of the last transmit report

/* Interval between two transmit progress reports, in milliseconds */
#define TX_REPORT_INTERVAL 500

/**
 * Read back what was written during the initialisation. A mismatch here means
 * the SPI interface towards the baseband is not working at all, which is worth
 * telling apart from a chip which is configured but receiving nothing.
 */
static void checkInitRegisters()
{
    uint8_t timing = C6000.getDmrTiming();
    bool ok = (timing & C6000_TIMING_RX_MASK) == C6000_TIMING_RX_MASK;

    /*
     * Only the receiver bits are checked. The transmit enable does not read
     * back while the transmitter is idle, and the color code register does not
     * read back at all, so neither can be confirmed here.
     */
    dmrDebug_log(DMR_DBG_INFO,
                 "init: timing %02X, receiver bits %s, "
                 "color code set to %u",
                 timing, ok ? "PASS" : "FAIL", configuredCc);

    /* The registers shaping DMR reception, to confirm the mode setup took. */
    dmrDebug_log(DMR_DBG_INFO,
                 "init: regs 01=%02X 06=%02X 10=%02X 11=%02X 26=%02X 34=%02X "
                 "5F=%02X 81=%02X E0=%02X",
                 C6000.readCfgRegister(0x01), C6000.readCfgRegister(0x06),
                 C6000.readCfgRegister(0x10), C6000.readCfgRegister(0x11),
                 C6000.readCfgRegister(0x26), C6000.readCfgRegister(0x34),
                 C6000.readCfgRegister(0x5F), C6000.readCfgRegister(0x81),
                 C6000.readCfgRegister(0xE0));
}

/**
 * Report how often each interrupt line fired since the last report. With the
 * receiver idling in passive timing the lines are expected to be quiet until
 * a signal comes in; system events staying at zero during a transmission on
 * frequency is the failure signature.
 */
static void reportIrqActivity()
{
    long long now = getTick();

    if ((now - reportTick) < IRQ_REPORT_INTERVAL)
        return;

    struct dmrIrqCounters counters;
    dmrIrq_getCounters(&counters);

    if (dmrIrq_stormDetected())
        dmrDebug_log(DMR_DBG_INFO, "irq: lines shut down, firing continuously");

    dmrIrq_feedWatchdog();

    dmrDebug_log(DMR_DBG_IRQ,
                 "irq: timeslot %lu system %lu transmit %lu in "
                 "%lums, flags %02X status %02X lines low %X",
                 (unsigned long)(counters.timeslot - reportedTs),
                 (unsigned long)(counters.system - reportedSys),
                 (unsigned long)(counters.transmit - reportedTx),
                 (unsigned long)(now - reportTick), seenFlags, seenStatus,
                 seenLinesLow);

    seenFlags = 0;
    seenStatus = 0;
    seenLinesLow = 0;

    reportedTs = counters.timeslot;
    reportedSys = counters.system;
    reportedTx = counters.transmit;
    reportTick = now;
}

#if defined(PLATFORM_MDUV3x0)

/*
 * The TYT firmware reads the slot's colour code as soon as the timeslot
 * interrupt comes in, and restarts the receiver after eight slots in a row
 * without the configured colour code. During a call the idle slot reads a
 * random colour code, so one slot in two misses and the count never gets
 * there; once the carrier goes it does within 240ms.
 */
#define SLOT_READ_DELAY 0
#define SLOT_MISS_LIMIT 8

/*
 * The TYT firmware sets the DMR mode up again at the end of every
 * transmission: left as the transmission leaves it, the chip detects no sync
 * until the mode is written again. On the bench the protocol half alone was
 * not enough: the board's front end has to be written again too, as leaving
 * DMR mode and coming back does.
 */
#define MODE_SETUP_AFTER_TX true

/*
 * The TYT firmware leaves the chip's vocoder registers, 0x20 to 0x22, as its
 * initialisation set them for the whole of DMR reception, and reads the clear
 * voice payload whatever colour code the burst carrying it reported: only the
 * slots are judged on the colour code.
 */
#define RX_VOCODER_CONTROL false
#define VOICE_NEEDS_COLOR_CODE false

/*
 * No reception through a repeater yet: the TYT firmware's repeater paths are
 * not decoded. A repeater channel runs direct mode.
 */
#define REPEATER_RX 0

#else

#define MODE_SETUP_AFTER_TX false
#define RX_VOCODER_CONTROL true
#define VOICE_NEEDS_COLOR_CODE true

/* Reception through a repeater, the way the Connect Systems firmware does it. */
#define REPEATER_RX 1

/*
 * Delay between the timeslot interrupt and the read of the slot status, by
 * when the chip has updated it for the slot just gone, in milliseconds. The
 * Connect Systems firmware waits the same.
 */
#define SLOT_READ_DELAY 10

/*
 * Bad slots in a row that drop the lock, and empty slots in a row that restart
 * the search while unlocked. During a call the idle slot reads a random colour
 * code, so a locked receiver sees one bad slot in two and never gets to four;
 * once the carrier goes both are bad and the lock drops within 120ms.
 */
#define LOCKED_MISS_LIMIT 4
#define UNLOCKED_MISS_LIMIT 12

/*
 * Slots in a row without a voice sync after which a call is lost: once a call
 * is accepted the stock firmware judges the slots on the frame A sync alone,
 * which comes once per superframe, every twelve slots. Twenty-five slots, about
 * 750ms, are two superframes without one.
 */
#define CALL_SYNC_MISS_LIMIT 25

#endif

#define RX_THREAD_STKSIZE 4096

static pthread_t rxThread;
static pthread_mutex_t rxMutex = PTHREAD_MUTEX_INITIALIZER; // Guards rxState
static volatile bool rxThreadRun;
static bool rxLocked;           // Receiver locked to a transmission
static uint8_t rxMisses;        // Bad or empty slots in a row
static uint8_t callSyncMisses;  // Call slots in a row without a voice sync
static struct dmrRxCall rxCall; // The call being received
static struct dmrRptSlot rpt;   // A repeater's downlink, in repeater mode
static bool rptBlocksSwapped;   // Our timeslot is in the other payload block
static volatile bool channelChanged; // Chip set up owed for a new channel

/**
 * Tell whether the channel goes through a repeater, on a radio that can.
 */
static inline bool repeaterMode()
{
    return REPEATER_RX && (config.access == DMR_ACCESS_REPEATER);
}

/**
 * Hand the state of the call being received to the rtx thread.
 */
static void publishCall()
{
    pthread_mutex_lock(&rxMutex);
    rxState.dstId = rxCall.call.dstId;
    rxState.srcId = rxCall.call.srcId;
    rxState.colorCode = rxCall.colorCode;
    rxState.groupCall = rxCall.call.groupCall;
    rxState.encrypted = rxCall.call.encrypted;
    rxState.valid = rxCall.active;
    pthread_mutex_unlock(&rxMutex);
}

/**
 * Restart the receiver's search for a transmission, as the stock firmware does
 * whenever it loses the lock or a call ends: stop reception, reset the slot
 * state machine, ask for the next slot and start reception again.
 */
static void armReceiver()
{
    C6000.dmrArmReceiver();

    rxLocked = false;
    rxMisses = 0;
}

/**
 * End the call in progress, in the order the stock firmware does at a
 * terminator: stop the chip's vocoder decode, restart the search for a
 * transmission, then clear the decode buffer.
 *
 * @param reason: what ended the call, for the log.
 */
static void endCall(const char *reason)
{
    if (RX_VOCODER_CONTROL)
        C6000.setDmrVocoder(C6000_DmrVocoder::OFF);

    /*
     * Through a repeater the alignment outlives the call, so that its hang
     * time is heard: the stock firmware does not restart the receiver here.
     */
    if (repeaterMode() == false)
        armReceiver();

    if (RX_VOCODER_CONTROL) {
        C6000.resetDmrVoiceControl();
        C6000.clearDmrBuffers(C6000_DmrBuffers::DECODE);
    }

    rxCall.active = false;
    publishCall();
    lastPayloadValid = false;

    dmrDebug_log(DMR_DBG_INFO, "call: ended, %s", reason);
}

/**
 * Lose a repeater's downlink, as the stock firmware's repeater reset does: end
 * any call, restart the receiver and search for the downlink's alignment again.
 *
 * @param reason: what lost it, for the log.
 */
static void restartRepeater(const char *reason)
{
    if (rxCall.active)
        endCall(reason);

    armReceiver();
    dmrRpt_reset(&rpt, config.timeslot, configuredCc);
    dmrDebug_log(DMR_DBG_INFO, "repeater: downlink lost, %s", reason);
}

/**
 * Read the voice payload of a burst received through a repeater. The chip
 * keeps a block per timeslot of the downlink, and can keep ours in the other
 * one: at each frame A, as the stock firmware does, the block holding the
 * repeater's voice sync tells which.
 *
 * @param payload: destination buffer, VP_LENGTH bytes.
 * @param status: burst status, register 0x51.
 */
static void readRepeaterPayload(uint8_t *payload, const uint8_t status)
{
    uint8_t block = (config.timeslot == 2) ? 1 : 0;
    if (rptBlocksSwapped)
        block ^= 1;

    if (((status & C6000_DMR_SYNC_MASK) == C6000_DMR_CLASS_VOICE_SYNC)
        && (C6000.dmrRepeaterVoiceSync(block) == false)
        && C6000.dmrRepeaterVoiceSync(block ^ 1)) {
        rptBlocksSwapped = !rptBlocksSwapped;
        block ^= 1;
        dmrDebug_log(DMR_DBG_LC, "repeater: payload in block %u", block);
    }

    C6000.readDmrRepeaterPayload(payload, block);
}

/**
 * Read the link control the chip has just decoded and check it is a voice
 * call. The chip has already checked it, so it is trusted as it comes.
 *
 * @param lc: destination buffer, C6000_LC_LENGTH bytes long.
 * @return true for the link control of a group or private voice call.
 */
static bool readVoiceLc(uint8_t *lc)
{
    C6000.readDmrLinkControl(lc);
    dmrDebug_record(DMR_DBG_LC, "LC", lc, C6000_LC_LENGTH);

    struct dmrVoiceCall call;
    if (dmr_parseVoiceLc(lc, &call) == false) {
        dmrDebug_log(DMR_DBG_LC, "lc: ignored, FLCO %02X", dmr_lcOpcode(lc));
        return false;
    }

    return true;
}

/**
 * Take the link control of a voice LC header or of a superframe's embedded
 * signalling: start the call it describes, or keep the one in progress.
 *
 * @param cc: colour code the link control was received with.
 */
static void takeCallLc(const uint8_t cc)
{
    uint8_t lc[C6000_LC_LENGTH];
    if (readVoiceLc(lc) == false)
        return;

    if (dmrCall_takeLc(&rxCall, lc, cc, getTick()) != DMR_CALL_STARTED)
        return;

    callSyncMisses = 0;

    /*
     * A new call: clear the chip's decode buffer as the stock firmware does
     * when it accepts one.
     */
    if (RX_VOCODER_CONTROL)
        C6000.clearDmrBuffers(C6000_DmrBuffers::DECODE);

    publishCall();
    lastPayloadValid = false;

    const struct dmrVoiceCall *call = &rxCall.call;
    dmrDebug_log(DMR_DBG_INFO, "call: from %lu to %s %lu, color code %u%s",
                 (unsigned long)call->srcId, call->groupCall ? "TG" : "ID",
                 (unsigned long)call->dstId, cc,
                 call->encrypted ? ", encrypted" : "");
}

/**
 * Take a terminator: end the call in progress if the terminator is its own.
 */
static void takeTerminator()
{
    uint8_t lc[C6000_LC_LENGTH];
    if ((readVoiceLc(lc) == false) || (rxCall.active == false))
        return;

    if (dmrCall_takeTerminator(&rxCall, lc) == DMR_CALL_ENDED)
        endCall("terminator");
}

/**
 * Take the payload of a voice burst of the call in progress and queue its
 * three AMBE frames for playback.
 *
 * @param status: burst status the payload was received with, for the tap.
 */
static void takeVoicePayload(const uint8_t status)
{
    /*
     * Received voice is read from the chip's data page on both radios, as
     * their stock firmwares do in clear voice: the payload bus returns zeros
     * on the CS7000 Plus and a stale payload on the MD-UV3x0.
     */
    uint8_t payload[VP_LENGTH];
    if (repeaterMode())
        readRepeaterPayload(payload, status);
    else
        C6000.readDmrVoicePayload(payload);

    if (dmrDebug_channelEnabled(DMR_DBG_VP)) {
        uint8_t record[1 + VP_LENGTH];
        record[0] = status;
        memcpy(&record[1], payload, VP_LENGTH);
        dmrDebug_record(DMR_DBG_VP, "VP", record, sizeof(record));
    }

    bool repeated = lastPayloadValid
                 && (memcmp(payload, lastPayload, VP_LENGTH) == 0);
    memcpy(lastPayload, payload, VP_LENGTH);
    lastPayloadValid = true;

    if (repeated)
        return;

    /*
     * Every frame goes to playback, as the stock firmwares do: the vocoder's
     * FEC corrects what it can and conceals the rest. A frame left out would
     * leave a 20 ms gap in the audio instead.
     */
    for (int f = 0; f < 3; f++)
        dmrAudio_pushFrame(&payload[f * DMR_CODEC_FRAME_BYTES]);
}

/**
 * Handle a received and parsed burst. Every burst carrying a colour code has
 * to come with the configured one: the chip decodes both slots in direct mode,
 * and the idle one reads a random colour code.
 *
 * @param status: burst status, register 0x51, read at the event.
 * @param cc: received colour code, register 0x52, read at the event.
 */
static void handleBurstEvent(const uint8_t status, const uint8_t cc)
{
    dmrDebug_log(DMR_DBG_LC, "burst: status %02X cc %u", status, cc);

    enum dmrBurstKind kind;
    switch (status & C6000_DMR_SYNC_MASK) {
        case C6000_DMR_CLASS_DATA:
            kind = DMR_BURST_DATA;
            break;
        case C6000_DMR_CLASS_VOICE:
            kind = DMR_BURST_VOICE;
            break;
        case C6000_DMR_CLASS_VOICE_SYNC:
            kind = DMR_BURST_VOICE_SYNC;
            break;
        default:
            return;
    }

    bool ccChecked = dmr_burstHasColorCode(kind)
                  && ((kind == DMR_BURST_DATA) || VOICE_NEEDS_COLOR_CODE);
    if (ccChecked && (cc != configuredCc))
        return;

    if (kind == DMR_BURST_DATA) {
        if ((status >> 4) == C6000_DMR_DATA_VOICE_HEADER)
            takeCallLc(cc);
        else if ((status >> 4) == C6000_DMR_DATA_TERMINATOR)
            takeTerminator();
        return;
    }

    if (dmrCall_takeVoice(&rxCall, getTick()) == false)
        return;

    /* Encrypted voice is not played, as in OpenGD77. */
    if (rxCall.call.encrypted == false)
        takeVoicePayload(status);
}

/**
 * Handle a system interrupt: read the flags and the burst they describe, then
 * handle and acknowledge the flags one at a time. Until the receiver has locked
 * to a transmission the bursts are noise and are only acknowledged.
 */
static void handleSystemEvent()
{
    struct C6000_DmrStatus status;
    C6000.readDmrStatus(&status);

    seenFlags |= status.intFlags;
    seenStatus |= status.rxStatus;

    uint8_t cc = (status.cach >> 4) & 0x0F;

    for (uint8_t flag : C6000_DMR_FLAG_ORDER) {
        if ((status.intFlags & flag) == 0)
            continue;

        /* Checked per flag, as handling a terminator ends the call. */
        bool active = rxLocked || rxCall.active;

        if (active && (flag == C6000_DMR_FLAG_BURST))
            handleBurstEvent(status.rxStatus, cc);
        else if (active && (flag == C6000_DMR_FLAG_EMBEDDED_LC))
            takeCallLc(cc);

        C6000.clearDmrInterrupts(flag);
    }
}

#if defined(PLATFORM_MDUV3x0)

/**
 * Run the TYT firmware's slot handler, once per slot: the receiver counts as
 * aligned from the first slot boundary on, each slot is judged on its colour
 * code alone, and eight misses in a row restart the search for a
 * transmission.
 */
static void handleSlot()
{
    uint8_t cach = C6000.readCfgRegister(0x52);
    uint8_t cc = (cach >> 4) & 0x0F;

    rxLocked = true;

    if (cc == configuredCc) {
        rxMisses = 0;
    } else {
        rxMisses += 1;
        if (rxMisses >= SLOT_MISS_LIMIT) {
            dmrDebug_log(DMR_DBG_LC, "slot: no colour code %u, re-armed",
                         configuredCc);
            if (rxCall.active)
                endCall("carrier lost");
            else
                armReceiver();

            return;
        }
    }

    dmrDebug_log(DMR_DBG_LC, "slot: 52=%02X misses %u", cach, rxMisses);
    C6000.setDmrSlotAction(C6000_DmrSlotAction::NEXT);
}

#else

static bool slotParity;        // Set after the station's slots
static uint32_t lastSlotCount; // Timeslot interrupts at the last slot read

/**
 * Run the Connect Systems firmware's repeater slot handler, once per slot.
 * While searching, align on the downlink's CACH TC bits; once aligned,
 * receive our timeslot and leave the other one out. A call in progress is
 * judged on its own frame A sync, as in direct mode, and keeps the alignment
 * when it ends; losing the call, or the downlink, starts the search over.
 */
static void handleRepeaterSlot()
{
    uint8_t status = C6000.readCfgRegister(0x51);
    uint8_t cach = C6000.readCfgRegister(0x52);
    uint8_t syncSource = C6000.readCfgRegister(0x5F);

    struct dmrIrqCounters counters;
    dmrIrq_getCounters(&counters);
    uint32_t slots = counters.timeslot - lastSlotCount;
    lastSlotCount = counters.timeslot;

    dmrDebug_log(DMR_DBG_LC, "slot %lu: 51=%02X 52=%02X 5F=%02X",
                 (unsigned long)counters.timeslot, status, cach, syncSource);

    if (rxCall.active) {
        if ((status & C6000_DMR_SYNC_MASK) == C6000_DMR_CLASS_VOICE_SYNC) {
            callSyncMisses = 0;
        } else {
            callSyncMisses += 1;
            if (callSyncMisses >= CALL_SYNC_MISS_LIMIT) {
                restartRepeater("no voice sync");
                return;
            }
        }

        bool ours = (dmrRpt_advance(&rpt, slots) == DMR_RPT_TAKE);
        C6000.setDmrSlotAction(ours ? C6000_DmrSlotAction::RECEIVE :
                                      C6000_DmrSlotAction::IDLE);
        return;
    }

    struct dmrRptSample sample;
    sample.repeater = (syncSource & 0x03) == 1;
    sample.checkFailed = (status & C6000_DMR_CHECK_FAILED) != 0;
    sample.colorCode = (cach >> 4) & 0x0F;
    sample.tc = (cach >> 2) & 1;

    bool wasAligned = rpt.aligned;
    switch (dmrRpt_takeSlot(&rpt, &sample, slots)) {
        case DMR_RPT_LOST:
            if (wasAligned)
                restartRepeater(sample.repeater ? "no colour code" :
                                                  "not a repeater");
            else
                armReceiver();
            return;

        case DMR_RPT_SKIP:
            C6000.setDmrSlotAction(C6000_DmrSlotAction::IDLE);
            break;

        case DMR_RPT_TAKE:
            C6000.setDmrSlotAction(C6000_DmrSlotAction::NEXT);
            break;

        case DMR_RPT_SEARCH:
            break;
    }

    if (rpt.aligned && (wasAligned == false)) {
        rptBlocksSwapped = false;
        dmrDebug_log(DMR_DBG_INFO, "repeater: aligned on timeslot %u",
                     config.timeslot);
    }

    /* Bursts count only once aligned, as the stock firmware's lock does. */
    rxLocked = rpt.aligned;
}

/**
 * Run the Connect Systems firmware's slot handler, once per slot: lock to a
 * transmission on a frame sync, or on a data sync with the configured colour
 * code, keep receiving both slots while locked, and restart the search once
 * the lock is lost or nothing shows up for a while. Once a call is accepted,
 * receive only the slot it is sent in.
 *
 * Which slot that is comes from a parity the stock firmware toggles at every
 * slot boundary and sets whenever the slot just gone carried the station's
 * sync: from then on it is set at the boundaries that follow the station's
 * slots, where the slot action written applies to the station's next slot.
 * It advances here by the number of slot interrupts since the last read, so
 * that a slot handled late cannot swap the two.
 */
static void handleSlot()
{
    if (repeaterMode()) {
        handleRepeaterSlot();
        return;
    }

    uint8_t status, cc;
    C6000.readDmrBurst(&status, &cc);
    uint8_t sync = status & C6000_DMR_SYNC_MASK;
    bool ccOk = (cc == configuredCc);
    bool stationSync = (sync == C6000_DMR_CLASS_VOICE_SYNC)
                    || ((sync == C6000_DMR_CLASS_DATA) && ccOk);

    /*
     * The slot a call goes out in is told by its own bursts: its frame A, its
     * voice header or its terminator. A repeater's downlink also carries idle
     * data bursts, with the same colour code, on the other slot.
     */
    uint8_t dataType = status >> 4;
    bool callSync = (sync == C6000_DMR_CLASS_VOICE_SYNC)
                 || ((sync == C6000_DMR_CLASS_DATA) && ccOk
                     && ((dataType == C6000_DMR_DATA_VOICE_HEADER)
                         || (dataType == C6000_DMR_DATA_TERMINATOR)));

    struct dmrIrqCounters counters;
    dmrIrq_getCounters(&counters);
    if (((counters.timeslot - lastSlotCount) & 1) != 0)
        slotParity = !slotParity;
    lastSlotCount = counters.timeslot;

    /*
     * During a call only the frame A sync counts: the call is lost after two
     * superframes without one, however the other slots read.
     */
    if (rxCall.active) {
        if (sync == C6000_DMR_CLASS_VOICE_SYNC) {
            callSyncMisses = 0;
        } else {
            callSyncMisses += 1;
            if (callSyncMisses >= CALL_SYNC_MISS_LIMIT) {
                endCall("no voice sync");
                return;
            }
        }

        C6000.setDmrSlotAction(slotParity ? C6000_DmrSlotAction::RECEIVE :
                                            C6000_DmrSlotAction::IDLE);
        return;
    }

    if (rxLocked == false) {
        if (stationSync) {
            dmrDebug_log(DMR_DBG_LC, "slot: locked, status %02X cc %u", status,
                         cc);
            rxLocked = true;
            rxMisses = 0;
            slotParity = true;
            C6000.setDmrSlotAction(C6000_DmrSlotAction::NEXT);
        } else if (sync == C6000_DMR_CLASS_VOICE) {
            rxMisses += 1;
            if (rxMisses >= UNLOCKED_MISS_LIMIT) {
                dmrDebug_log(DMR_DBG_LC, "slot: no sync, re-armed");
                armReceiver();
            }
        }

        return;
    }

    bool good = stationSync || ((sync == C6000_DMR_CLASS_VOICE) && ccOk);

    if (good) {
        rxMisses = 0;
    } else {
        rxMisses += 1;
        if (rxMisses >= LOCKED_MISS_LIMIT) {
            dmrDebug_log(DMR_DBG_LC, "slot: lock lost, status %02X cc %u",
                         status, cc);
            armReceiver();
            return;
        }
    }

    if (callSync)
        slotParity = true;

    C6000.setDmrSlotAction(C6000_DmrSlotAction::NEXT);
}

#endif

/*
 * Transmission, as the stock firmware runs it from its timeslot task: every
 * other slot is sent, starting with three voice link control headers, the
 * last one starting the vocoder encode, then voice superframes until the
 * transmission is asked to stop after a frame F, a terminator, an idle slot,
 * and the end.
 */
#define TX_HEADERS 3                   // Voice headers opening a call
#define TX_STOP_TIMEOUT 1500           // Longest wind down after unkey, ms

static volatile bool txKeyupRequested; // Key-up handed to the thread
static uint32_t txSrcId;               // Own ID, sent as the source
static struct dmrTxSequence txSeq;     // Bursts of the transmission

/**
 * Key up, as the stock firmware does: clear voice, transmission with the
 * chip generating its own slot timing and the receiver off, starting on the
 * next slot. A call being received is dropped; direct mode transmission does
 * not wait for the channel in the stock firmware either.
 */
static void keyUp()
{
    txKeyupRequested = false;
    rxRestoreNeeded = false;

    if (rxCall.active) {
        rxCall.active = false;
        publishCall();
        lastPayloadValid = false;
        dmrDebug_log(DMR_DBG_INFO, "call: ended, transmitting");
    }

    dmrTx_start(&txSeq, TX_HEADERS);
    txSequence = 0;

    C6000.setDmrPrivacy(false);
    C6000.setDmrOwnId(txSrcId);
    C6000.dmrTransmitMode();
    C6000.setDmrTiming(C6000_DmrTiming::TRANSMIT);
    C6000.setDmrSlotAction(C6000_DmrSlotAction::NEXT);

    rxLocked = false;
    rxMisses = 0;

    dmrDebug_log(DMR_DBG_INFO, "tx: keyed, timing %02X", C6000.getDmrTiming());
}

/**
 * End the transmission, in the slot after the one following the terminator:
 * stop the vocoder encode, clear the vocoder buffers and stop transmitting.
 * The receiver comes back on the next event.
 *
 * @param reason: what ended it, for the log.
 */
static void endTransmission(const char *reason)
{
    C6000.setDmrVocoder(C6000_DmrVocoder::IDLE);
    C6000.clearDmrBuffers(C6000_DmrBuffers::ALL);
    C6000.setDmrSlotAction(C6000_DmrSlotAction::NEXT);

    txRunning = false;
    rxRestoreNeeded = true;

    dmrDebug_log(DMR_DBG_INFO, "tx: ended, %s", reason);
}

/**
 * Set up the next slot of the transmission, straight after the slot boundary.
 * The link control is written before the burst type, and the voice payload
 * before the burst it goes out in.
 */
static void txSlot()
{
    struct dmrTxBurst burst = dmrTx_nextSlot(&txSeq, txStopRequested);

    switch (burst.type) {
        case DMR_TX_IDLE:
            C6000.setDmrSlotAction(C6000_DmrSlotAction::IDLE);
            return;

        case DMR_TX_END:
            endTransmission("terminator sent");
            return;

        default:
            break;
    }

    C6000.setDmrSlotAction(C6000_DmrSlotAction::TRANSMIT);

    switch (burst.type) {
        case DMR_TX_HEADER:
            C6000.writeDmrLinkControl(txLc);
            C6000.setDmrBurstType(C6000_DmrBurstType::VOICE_HEADER);

            /* The vocoder starts encoding with the last header. */
            if (burst.lastHeader)
                C6000.setDmrVocoder(C6000_DmrVocoder::ENCODE);
            break;

        case DMR_TX_TERMINATOR:
            C6000.writeDmrLinkControl(txLc);
            C6000.setDmrBurstType(C6000_DmrBurstType::TERMINATOR);
            break;

        case DMR_TX_VOICE: {
            uint8_t payload[VP_LENGTH];
            nextVoicePayload(payload);

            dmrPayload_write(payload);
            C6000.setDmrBurstType(C6000_DmrBurstType::VOICE, burst.frame);
            txSequence = txSeq.frame;
            break;
        }

        default:
            break;
    }
}

/**
 * Acknowledge the chip's interrupt flags while transmitting, when they carry
 * nothing to act on.
 */
static void acknowledgeFlags()
{
    uint8_t flags = C6000.getDmrInterrupts();
    if (flags != 0)
        C6000.clearDmrInterrupts(flags);
}

/**
 * Bring the receiver back after a transmission, as the stock firmware does:
 * clear the vocoder buffers and reset its decode, or on the MD-UV3x0 set the
 * DMR mode up again, then restart the search for a transmission.
 */
static void restoreAfterTx()
{
    rxRestoreNeeded = false;

    if (MODE_SETUP_AFTER_TX) {
        C6000.dmrMode();
        C6000.dmrDirectMode();
        C6000.setDmrColorCode(configuredCc);
    } else {
        C6000.resetDmrVoiceControl();
        C6000.clearDmrBuffers(C6000_DmrBuffers::ENCODE);
        C6000.setDmrVocoder(C6000_DmrVocoder::OFF);
        C6000.setDmrVocoder(C6000_DmrVocoder::IDLE);
    }

    armReceiver();
    acknowledgeFlags();

    dmrDebug_log(DMR_DBG_INFO, "dmr: receiver restarted after tx, timing %02X",
                 C6000.getDmrTiming());
}

/**
 * The DMR thread: woken by the interrupt lines, it does all the chip access for
 * reception and transmission through the normal, mutex protected, accessors,
 * as the stock firmware does from the tasks its interrupts wake. System events
 * are handled straight away; the slot handler runs a fixed delay after each
 * slot boundary when receiving, straight away when transmitting.
 */
static void *rxThreadFunc(void *arg)
{
    (void)arg;
    long long slotDue = -1;

    while (rxThreadRun) {
        uint32_t events = (slotDue < 0) ? dmrIrq_waitEvents() :
                                          dmrIrq_takeEvents();
        if (rxThreadRun == false)
            break;

        if (txKeyupRequested) {
            keyUp();
            slotDue = -1;
            continue;
        }

        if (channelChanged) {
            channelChanged = false;
            if (rxCall.active)
                endCall("channel changed");

            setupChannel();
            C6000.setDmrColorCode(configuredCc);
            C6000.setDmrTiming(C6000_DmrTiming::RX_IDLE);
            armReceiver();
            reportAccess();
            slotDue = -1;
            continue;
        }

        if (rxRestoreNeeded) {
            restoreAfterTx();
            slotDue = -1;
            continue;
        }

        if (dmr_txActive()) {
            slotDue = -1;

            if ((events & DMR_IRQ_EVENT_SYSTEM) != 0)
                acknowledgeFlags();

            if ((events & DMR_IRQ_EVENT_TIMESLOT) != 0)
                txSlot();

            /*
             * The transmission winds down on its own once unkeyed, within a
             * superframe or so; if the slot clock stopped it would not.
             */
            if (dmr_txActive() && txStopRequested
                && ((getTick() - txStopTick) > TX_STOP_TIMEOUT))
                endTransmission("timed out after unkey");

            continue;
        }

        if ((events & DMR_IRQ_EVENT_SYSTEM) != 0)
            handleSystemEvent();

        /*
         * Once a call is accepted the stock firmware reads the slot straight
         * away, without the delay it waits before then.
         */
        if ((events & DMR_IRQ_EVENT_TIMESLOT) != 0)
            slotDue = getTick() + (rxCall.active ? 0 : SLOT_READ_DELAY);

        if ((slotDue >= 0) && (getTick() >= slotDue)) {
            handleSlot();
            slotDue = -1;
        }

        /*
         * A call whose bursts stopped without a terminator and without the
         * lock being dropped, which the slot handler normally catches first.
         */
        if (dmrCall_expired(&rxCall, getTick(), RX_HOLD_TIMEOUT)) {
            if (repeaterMode())
                restartRepeater("timeout");
            else
                endCall("timeout");
        }

        if (slotDue >= 0)
            sleepFor(0u, 1u);
    }

    return NULL;
}

/**
 * Build the payload of the next voice burst: three encoded AMBE frames from
 * the microphone, any not yet available falling back to silence; or, on the
 * bench, the test pattern: a marker byte, the sequence number and its
 * complement, filler, and a checksum over the rest.
 *
 * @param payload: destination, VP_LENGTH bytes.
 */
static void nextVoicePayload(uint8_t *payload)
{
    for (int f = 0; f < VP_LENGTH / DMR_CODEC_FRAME_BYTES; f++) {
        uint8_t *slot = &payload[f * DMR_CODEC_FRAME_BYTES];
        if (dmrAudio_popFrame(slot) == false)
            memcpy(slot, DMR_VOICE_SILENCE_FRAME, DMR_CODEC_FRAME_BYTES);
    }

    if (txMarker == false)
        return;

    memset(payload, 0x5A, VP_LENGTH);
    payload[0] = 0xA5;
    payload[1] = txMarkerSeq;
    payload[2] = ~txMarkerSeq;
    txMarkerSeq += 1;

    uint8_t sum = 0;
    for (int i = 0; i < VP_LENGTH - 1; i++)
        sum ^= payload[i];
    payload[VP_LENGTH - 1] = sum;
}

/**
 * Debug tap command: send the test pattern in place of the voice, or the voice
 * again. Each burst's payload then carries a sequence number, so a receiver on
 * the bench can tell whether every payload handed to the baseband went out on
 * air exactly once: chip select timing on the CS7000 Plus vocoder bus once
 * lost a share of them, each going out as a repeat of the previous burst.
 */
static void toggleMarker()
{
    txMarker = !txMarker;
    txMarkerSeq = 0;
    dmrDebug_log(DMR_DBG_INFO, "tx: test pattern %s", txMarker ? "on" : "off");
}

bool dmr_txActive()
{
    return txRunning;
}

void dmr_startTx(const bool groupCall, const uint32_t dstId)
{
    const uint32_t srcId = config.ownId;

    /*
     * The key-up itself is left to the DMR thread, which runs the whole
     * transmission, so that it never interleaves with reception.
     */
    dmr_buildVoiceLc(txLc, groupCall, dstId, srcId);
    txSrcId = srcId;
    txStopRequested = false;
    txRunning = true;

    struct dmrIrqCounters counters;
    dmrIrq_getCounters(&counters);
    txReportedTs = counters.timeslot;
    txReportedTx = counters.transmit;
    txReportTick = getTick();

    txKeyupRequested = true;
    dmrIrq_wakeWaiter();

    dmrDebug_log(DMR_DBG_INFO, "tx: keying, to %s %lu from %lu",
                 groupCall ? "TG" : "ID", (unsigned long)dstId,
                 (unsigned long)srcId);
}

void dmr_stopTx()
{
    /* The rtx layer asks on every step until the transmission is over. */
    if (txStopRequested == false) {
        txStopTick = getTick();
        dmrDebug_log(DMR_DBG_INFO, "tx: unkey requested");
    }

    txStopRequested = true;
}

void dmr_txTask()
{
    /*
     * The storm guard shuts the interrupt lines down when too many events pile
     * up between two feeds. During a transmission the timeslot interrupt runs
     * the slot clock at its normal rate, which is well within the limit, but
     * dmr_task(), which usually feeds the guard, is not called, so it is fed
     * from here instead.
     */
    dmrIrq_feedWatchdog();

    long long now = getTick();

    /* Let the thread check its wind down even if the slot clock stopped. */
    if (dmr_txActive() && txStopRequested
        && ((now - txStopTick) > TX_STOP_TIMEOUT))
        dmrIrq_wakeWaiter();

    if ((now - txReportTick) < TX_REPORT_INTERVAL)
        return;

    /*
     * Report the slot clock and the transmission's progress while sending. A
     * timeslot rate around 33Hz says the chip is driving the slots and the
     * voice frame sequence moving says the bursts are being set up; either
     * sitting still is the failure signature.
     */
    struct dmrIrqCounters counters;
    dmrIrq_getCounters(&counters);

    dmrDebug_log(DMR_DBG_IRQ,
                 "tx: state %u seq %u, timeslot %lu transmit %lu "
                 "in %lums",
                 txRunning ? 1u : 0u, txSequence,
                 (unsigned long)(counters.timeslot - txReportedTs),
                 (unsigned long)(counters.transmit - txReportedTx),
                 (unsigned long)(now - txReportTick));

    txReportedTs = counters.timeslot;
    txReportedTx = counters.transmit;
    txReportTick = now;
}

/**
 * Say on the tap what the configuration asks for that the baseband does not do
 * yet: repeater channels are received and sent in direct mode for now.
 */
static void reportAccess()
{
    if (config.access != DMR_ACCESS_REPEATER)
        return;

    if (REPEATER_RX)
        dmrDebug_log(DMR_DBG_INFO, "dmr: repeater channel, timeslot %u",
                     config.timeslot);
    else
        dmrDebug_log(DMR_DBG_INFO,
                     "dmr: repeater channel, timeslot %u: repeater mode is "
                     "not supported yet, running direct mode",
                     config.timeslot);
}

/**
 * Set the protocol side of the chip up for the channel, the way the radio's
 * stock firmware loads it: for direct mode, or for the repeater mode of the
 * channel's timeslot, and start following the downlink afresh.
 */
static void setupChannel()
{
#if REPEATER_RX
    if (config.access == DMR_ACCESS_REPEATER)
        C6000.dmrRepeaterMode(config.timeslot);
    else
        C6000.dmrDirectMode();
#else
    C6000.dmrDirectMode();
#endif

    dmrRpt_reset(&rpt, config.timeslot, configuredCc);
    rptBlocksSwapped = false;
}

void dmr_init(const struct dmrConfig *cfg)
{
    config = *cfg;

    memset(&rxState, 0, sizeof(rxState));
    lastPayloadValid = false;

    txRunning = false;
    txStopRequested = false;
    rxRestoreNeeded = false;

    configuredCc = config.colorCode & 0x0F;
    reportedTs = 0;
    reportedSys = 0;
    reportedTx = 0;
    reportTick = getTick();

    /*
     * Logged in stages: if the radio does not come back, the last line which
     * made it out says which of the two halves is at fault.
     */
    dmrDebug_mark("dmr: configuring baseband");

    /* The voice payload rides on its own SPI bus, brought up here. */
    dmrPayload_init();

    /*
     * The protocol side of the chip, set up the way the radio's stock firmware
     * does it; the radio driver has already set up the board's front end.
     */
    channelChanged = false;
    setupChannel();

    /*
     * The receiver idles in passive timing so the chip can lock to the slot
     * clock of whoever transmits. Active timing made it free run instead:
     * the timeslot line ticked at a steady 33Hz while a transmission on
     * frequency produced no system events at all.
     */
    C6000.setDmrColorCode(configuredCc);
    C6000.setDmrTiming(C6000_DmrTiming::RX_IDLE);

    armReceiver();
    dmrCall_reset(&rxCall);

    dmrDebug_mark("dmr: baseband configured");
    checkInitRegisters();

    dmrDebug_mark("dmr: enabling interrupt lines");
    dmrIrq_init();

    rxThreadRun = true;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
#if defined(_MIOSIX)
    pthread_attr_setstacksize(&attr, RX_THREAD_STKSIZE);
    struct sched_param param;
    param.sched_priority = THREAD_PRIO_RT;
    pthread_attr_setschedparam(&attr, &param);
#endif

    if (pthread_create(&rxThread, &attr, rxThreadFunc, NULL) != 0) {
        rxThreadRun = false;
        dmrDebug_log(DMR_DBG_INFO, "dmr: reception thread not started");
    }

    dmrDebug_setCommand('c', dmr_codec_selfTest, "vocoder self-test");
    dmrDebug_setCommand('m', toggleMarker, "tx test pattern on/off");

    dmrDebug_mark("dmr: receiver running");
    reportAccess();
}

void dmr_terminate()
{
    if (rxThreadRun) {
        rxThreadRun = false;
        dmrIrq_wakeWaiter();
        pthread_join(rxThread, NULL);
    }

    dmrIrq_terminate();

    txRunning = false;
    txStopRequested = false;
    rxRestoreNeeded = false;

    C6000.setDmrSlotAction(C6000_DmrSlotAction::IDLE);
    dmrPayload_terminate();
    memset(&rxState, 0, sizeof(rxState));

    dmrDebug_setCommand('c', NULL, NULL);
    dmrDebug_setCommand('m', NULL, NULL);

    dmrDebug_log(DMR_DBG_INFO, "dmr: receiver stopped");
}

void dmr_configure(const struct dmrConfig *cfg)
{
    bool accessChanged = (cfg->access != config.access)
                      || (cfg->timeslot != config.timeslot);
    bool ccChanged = (cfg->colorCode & 0x0F) != configuredCc;
    config = *cfg;
    configuredCc = config.colorCode & 0x0F;

    /*
     * Another access or timeslot sets the chip up again, and so does another
     * colour code through a repeater, as the downlink is followed on it. The
     * DMR thread does it, as it owns the receiver.
     */
    if (accessChanged || (ccChanged && repeaterMode())) {
        channelChanged = true;
        dmrIrq_wakeWaiter();
        return;
    }

    if (ccChanged == false)
        return;

    C6000.setDmrColorCode(configuredCc);
    dmrDebug_log(DMR_DBG_INFO, "dmr: color code set to %u", configuredCc);
}

void dmr_task(struct dmrRxState *rx)
{
    /*
     * Reception runs on its own thread: this hands its state out, keeps the
     * diagnostics going and wakes the thread for the work it would otherwise
     * only notice on its next event, as the slot clock may have stopped:
     * bringing the receiver back after a transmission, or ending a call whose
     * bursts are gone.
     */
    if (gpio_readPin(DMR_TS_INT) == 0)
        seenLinesLow |= 0x01;
    if (gpio_readPin(DMR_SYS_INT) == 0)
        seenLinesLow |= 0x02;
    if (gpio_readPin(DMR_TX_INT) == 0)
        seenLinesLow |= 0x04;

    if (rxRestoreNeeded || dmrCall_expired(&rxCall, getTick(), RX_HOLD_TIMEOUT))
        dmrIrq_wakeWaiter();

    reportIrqActivity();

    pthread_mutex_lock(&rxMutex);
    *rx = rxState;
    pthread_mutex_unlock(&rxMutex);
}

#endif /* CONFIG_DMR */
