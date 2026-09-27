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
#include "dmr.h"
#include "dmr_audio.h"
#include "dmrPayload.h"
#include "protocols/DMR/LinkControl.h"
#include "core/threads.h"
#include <pthread.h>
#include <string.h>

#if defined(CONFIG_DMR)

/*
 * The CS7000 Plus runs DMR from a thread of its own, woken by the chip's
 * interrupt lines, receiving and transmitting the way its stock firmware does.
 * The MD-UV3x0 still polls from the rtx thread and transmits from the timeslot
 * interrupt, until the same has been bench tested on it.
 */
#if defined(PLATFORM_CS7000P)
#define DMR_THREAD
#endif

/*
 * Bits of the receive status register, 0x51: the CRC bit is set when the check
 * failed, so a burst is good when it reads back as zero.
 */
#define RX_STATUS_CRC_ERROR     0x04
#define RX_STATUS_PRIVACY       0x08

/*
 * Interrupt flag the chip raises on every slot once its slot clock runs,
 * whether or not anything was received; any other flag is a received event.
 */
#define INT_FLAG_SLOT           0x20

/* Full link control frames of a group and of a private call */
#define LC_FLCO_GROUP           0x00
#define LC_FLCO_PRIVATE         0x03

/*
 * The voice payload of a burst: three 9-byte AMBE frames the chip hands over
 * on its second SPI bus, reached through the board's payload bus driver.
 */
#define VP_LENGTH               DMR_PAYLOAD_LENGTH

/*
 * The chip hands back the link control of the previous transmission until it
 * has decoded the current one, so the same content has to show up in more than
 * one burst before it can be trusted.
 */
#define LC_CONFIRM_COUNT        2

/*
 * Registers and values driving the transmit slot, written from the timeslot
 * interrupt. Register 0x41 says what the chip does in the upcoming slot, 0x50
 * the type of burst it builds. The burst types are a voice link control
 * header, a voice frame carrying its sequence number in the high nibble, and a
 * terminator with link control.
 */
#define TX_REG_SLOT_ACTION      0x41
#define TX_REG_BURST_TYPE       0x50
#define TX_SLOT_IDLE            0x00
#define TX_SLOT_TRANSMIT        0x80
#define TX_BURST_VOICE_HEADER   0x10
#define TX_BURST_VOICE          0x08
#define TX_BURST_TERMINATOR     0x20

/* Number of voice frames in a superframe, the burst sequence cycles 0..5. */
#define TX_SUPERFRAME_FRAMES    6

/*
 * A voice frame worth of AMBE silence: the fixed public pattern radios and
 * hotspots send when there is no speech, three 9-byte frames making up the
 * 27-byte payload. Hardcoding it keeps the transmit path free of any vocoder
 * output; it is the same motif seen in our own received captures.
 */
static const uint8_t TX_SILENCE_AUDIO[VP_LENGTH] =
{
    0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B,
    0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B,
    0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B
};

/*
 * The transmit slot state machine, advanced one step per timeslot interrupt.
 * The call opens with three voice link control headers on alternating slots,
 * then voice superframes carrying the silence payload, and closes on a
 * terminator once the transmission has been asked to stop at a superframe
 * boundary. The gap steps leave the other slot idle, as a direct mode station
 * transmits on a single slot.
 */
enum dmrTxState
{
    TX_IDLE = 0,        ///< Not transmitting.
    TX_START_1,         ///< First header burst.
    TX_START_2,         ///< Idle slot.
    TX_START_3,         ///< Second header burst.
    TX_START_4,         ///< Idle slot.
    TX_START_5,         ///< Third header burst.
    TX_GAP,             ///< Idle slot between voice frames.
    TX_VOICE,           ///< Voice frame carrying the silence payload.
    TX_END_1,           ///< Terminator burst.
    TX_END_2,           ///< Terminator going out on air.
    TX_END_3            ///< Return to reception.
};

/* Interval between two interrupt activity reports, in milliseconds */
#define IRQ_REPORT_INTERVAL     1000

/*
 * How long the receive state is held after the last good burst before the
 * call is considered over, in milliseconds. Voice bursts on the received
 * slot are 60ms apart, so this rides out a few corrupted bursts without
 * keeping the call on screen after the carrier is gone.
 */
#define RX_HOLD_TIMEOUT         500

static struct dmrRxState rxState;
static uint8_t   configuredCc;
static uint8_t   prevLc[C6000_LC_LENGTH];
static uint8_t   lcMatches;
static uint32_t  lastSysCount;
static uint32_t  lastTsCount;
static long long lastBurstTick;
static uint32_t  reportedTs;
static uint32_t  reportedSys;
static uint32_t  reportedTx;
static long long reportTick;

/*
 * Chip interrupt flags (register 0x82) and burst status (register 0x51) seen
 * by polling since the last activity report, and which interrupt lines were
 * seen low. They tell a chip that detects nothing apart from interrupts that
 * never reach the processor.
 */
/*
 * Payload last handed to playback. The chip keeps a burst's payload until the
 * next burst replaces it and may raise more than one event per burst, so the
 * same payload can be read several times over.
 */
static uint8_t   lastPayload[VP_LENGTH];
static bool      lastPayloadValid;

static uint8_t   seenFlags;
static uint8_t   seenStatus;
static uint8_t   seenLinesLow;

static volatile uint8_t txState;        // Current transmit slot state
static volatile uint8_t txSequence;     // Voice frame sequence, 0..5
static volatile bool    txStopRequested;// Unkey asked, waiting for a boundary
static long long        txStopTick;     // When the unkey was asked
static volatile bool    rxRestoreNeeded;// Receiver bring-up owed after a TX
static uint8_t          txLc[DMR_LC_LENGTH]; // Link control sent in the bursts
static uint32_t  txReportedTs;          // Timeslot count at the last tx report
static uint32_t  txReportedTx;          // Transmit count at the last tx report
static long long txReportTick;          // Tick of the last transmit report

/* Interval between two transmit progress reports, in milliseconds */
#define TX_REPORT_INTERVAL      500

/**
 * Read back what was written during the initialisation. A mismatch here means
 * the SPI interface towards the baseband is not working at all, which is worth
 * telling apart from a chip which is configured but receiving nothing.
 */
static void checkInitRegisters()
{
    uint8_t timing = C6000.getDmrTiming();
    bool    ok     = (timing & C6000_TIMING_RX_MASK) == C6000_TIMING_RX_MASK;

    /*
     * Only the receiver bits are checked. The transmit enable does not read
     * back while the transmitter is idle, and the color code register does not
     * read back at all, so neither can be confirmed here.
     */
    dmrDebug_log(DMR_DBG_INFO, "init: timing %02X, receiver bits %s, "
                 "color code set to %u", timing, ok ? "PASS" : "FAIL",
                 configuredCc);

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

    if((now - reportTick) < IRQ_REPORT_INTERVAL)
        return;

    struct dmrIrqCounters counters;
    dmrIrq_getCounters(&counters);

    if(dmrIrq_stormDetected())
        dmrDebug_log(DMR_DBG_INFO, "irq: lines shut down, firing continuously");

    dmrIrq_feedWatchdog();

    dmrDebug_log(DMR_DBG_IRQ, "irq: timeslot %lu system %lu transmit %lu in "
                 "%lums, flags %02X status %02X lines low %X",
                 (unsigned long) (counters.timeslot - reportedTs),
                 (unsigned long) (counters.system   - reportedSys),
                 (unsigned long) (counters.transmit - reportedTx),
                 (unsigned long) (now - reportTick),
                 seenFlags, seenStatus, seenLinesLow);

    seenFlags    = 0;
    seenStatus   = 0;
    seenLinesLow = 0;

    reportedTs  = counters.timeslot;
    reportedSys = counters.system;
    reportedTx  = counters.transmit;
    reportTick  = now;
}

#if defined(DMR_THREAD)

/*
 * Burst status, register 0x51, as the stock firmware reads it: the sync class
 * in bits 1..0 and, in bit 2, whether the check on the burst failed; masking
 * both together gives a class only for a burst that passed. A data burst has
 * its data type in bits 7..4, a voice burst its frame number, A = 1 to F = 6,
 * in bits 6..4.
 */
#define RX_SYNC_MASK            0x07
#define RX_CLASS_VOICE          0x00    // Voice, frames B to F
#define RX_CLASS_VOICE_SYNC     0x01    // Voice carrying the sync, frame A
#define RX_CLASS_DATA           0x02    // Data or control, with its sync
#define RX_DATA_VOICE_HEADER    0x01    // Data type of a voice LC header
#define RX_DATA_TERMINATOR      0x02    // Data type of a terminator with LC

/*
 * Interrupt flags, register 0x82. Two carry work in reception: a burst was
 * received and parsed, and the embedded link control of a superframe is
 * complete, which is what identifies a call joined part way through. The
 * others are only acknowledged, 0x20 being the slot clock ticking. The stock
 * firmware handles and clears them one at a time, in this order.
 */
#define INT_FLAG_BURST          0x08
#define INT_FLAG_EMBEDDED_LC    0x10

static const uint8_t INT_FLAG_ORDER[] =
{
    0x80, 0x08, 0x40, 0x20, 0x10, 0x04, 0x02, 0x01
};

/* Slot action receiving the next slot; in direct mode both slots are taken. */
#define RX_SLOT_RECEIVE         0x40

/* Slot timing register value with the receiver stopped. */
#define RX_TIMING_OFF           0x03

/* Link control service options bit marking an encrypted call. */
#define LC_SO_PRIVACY           0x40

/*
 * Delay between the timeslot interrupt and the read of the slot status, by
 * when the chip has updated it for the slot just gone, in milliseconds. The
 * stock firmware waits the same.
 */
#define SLOT_READ_DELAY         10

/*
 * Bad slots in a row that drop the lock, and empty slots in a row that restart
 * the search while unlocked. During a call the idle slot reads a random colour
 * code, so a locked receiver sees one bad slot in two and never gets to four;
 * once the carrier goes both are bad and the lock drops within 120ms.
 */
#define LOCKED_MISS_LIMIT       4
#define UNLOCKED_MISS_LIMIT     12

#define RX_THREAD_STKSIZE       4096

static pthread_t       rxThread;
static pthread_mutex_t rxMutex = PTHREAD_MUTEX_INITIALIZER; // Guards rxState
static volatile bool   rxThreadRun;
static bool            rxLocked;    // Receiver locked to a transmission
static uint8_t         rxMisses;    // Bad or empty slots in a row
static bool            rxEncrypted; // The call in progress is encrypted

/**
 * Restart the receiver's search for a transmission, as the stock firmware does
 * whenever it loses the lock or a call ends: stop reception, reset the slot
 * state machine, ask for the next slot and start reception again.
 */
static void armReceiver()
{
    C6000.setDmrSlotAction(C6000_DmrSlotAction::IDLE);
    C6000.writeCfgRegister(0x40, RX_TIMING_OFF);
    C6000.setDmrSlotAction(C6000_DmrSlotAction::RESET);
    C6000.writeCfgRegister(TX_REG_SLOT_ACTION, RX_SLOT_RECEIVE);
    C6000.setDmrTiming(C6000_DmrTiming::RX_IDLE);

    rxLocked = false;
    rxMisses = 0;
}

/**
 * End the call in progress, in the order the stock firmware does at a
 * terminator: stop the chip's vocoder decode, restart the search for a
 * transmission, clear the decode buffer, then stop the playback.
 *
 * @param reason: what ended the call, for the log.
 */
static void endCall(const char *reason)
{
    C6000.writeCfgRegister(0x22, 0x16);
    armReceiver();
    C6000.writeCfgRegister(0x20, 0x00);
    C6000.writeCfgRegister(0x21, 0x01);

    pthread_mutex_lock(&rxMutex);
    rxState.valid = false;
    pthread_mutex_unlock(&rxMutex);

    rxEncrypted      = false;
    lastPayloadValid = false;

    dmrAudio_stopRx();
    dmrDebug_log(DMR_DBG_INFO, "call: ended, %s", reason);
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

    uint8_t flco = lc[0] & 0x3F;
    if((flco != LC_FLCO_GROUP) && (flco != LC_FLCO_PRIVATE))
    {
        dmrDebug_log(DMR_DBG_LC, "lc: ignored, FLCO %02X", flco);
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
    if(readVoiceLc(lc) == false)
        return;

    uint32_t dstId     = (lc[3] << 16) | (lc[4] << 8) | lc[5];
    uint32_t srcId     = (lc[6] << 16) | (lc[7] << 8) | lc[8];
    bool     group     = ((lc[0] & 0x3F) == LC_FLCO_GROUP);
    bool     encrypted = ((lc[2] & LC_SO_PRIVACY) != 0);

    lastBurstTick = getTick();

    if(rxState.valid && (rxState.srcId == srcId) && (rxState.dstId == dstId)
       && (rxState.groupCall == group))
        return;

    /*
     * A new call: clear the chip's decode buffer as the stock firmware does
     * when it accepts one, and restart the playback on a clean queue.
     */
    C6000.writeCfgRegister(0x21, 0x01);

    if(dmrAudio_running())
        dmrAudio_stopRx();

    pthread_mutex_lock(&rxMutex);
    rxState.dstId     = dstId;
    rxState.srcId     = srcId;
    rxState.colorCode = cc;
    rxState.groupCall = group;
    rxState.valid     = true;
    pthread_mutex_unlock(&rxMutex);

    rxEncrypted      = encrypted;
    lastPayloadValid = false;

    dmrDebug_log(DMR_DBG_INFO, "call: from %lu to %s %lu, color code %u%s",
                 (unsigned long) srcId, group ? "TG" : "ID",
                 (unsigned long) dstId, cc, encrypted ? ", encrypted" : "");

    /* Encrypted voice is not played, as in OpenGD77. */
    if(encrypted)
        return;

    if(dmrAudio_startRx())
        dmrDebug_log(DMR_DBG_INFO, "audio: voice playback started");
}

/**
 * Take a terminator: end the call in progress if the terminator is its own.
 */
static void takeTerminator()
{
    uint8_t lc[C6000_LC_LENGTH];
    if((readVoiceLc(lc) == false) || (rxState.valid == false))
        return;

    uint32_t dstId = (lc[3] << 16) | (lc[4] << 8) | lc[5];
    uint32_t srcId = (lc[6] << 16) | (lc[7] << 8) | lc[8];

    if((rxState.srcId == srcId) && (rxState.dstId == dstId))
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
    uint8_t payload[VP_LENGTH];
    dmrPayload_read(payload);

    if(dmrDebug_channelEnabled(DMR_DBG_VP))
    {
        uint8_t record[1 + VP_LENGTH];
        record[0] = status;
        memcpy(&record[1], payload, VP_LENGTH);
        dmrDebug_record(DMR_DBG_VP, "VP", record, sizeof(record));
    }

    bool repeated = lastPayloadValid
                    && (memcmp(payload, lastPayload, VP_LENGTH) == 0);
    memcpy(lastPayload, payload, VP_LENGTH);
    lastPayloadValid = true;

    if(repeated || (dmrAudio_running() == false))
        return;

    for(int f = 0; f < 3; f++)
    {
        const uint8_t *frame = &payload[f * DMR_CODEC_FRAME_BYTES];
        if(dmr_codec_frameIsVoice(frame))
            dmrAudio_pushFrame(frame);
    }
}

/**
 * Handle a received and parsed burst. Everything but the voice frame carrying
 * the sync has to come with the configured colour code: the chip decodes both
 * slots in direct mode, and the idle one reads a random colour code. At frame A
 * the colour code register is not updated, so it is not checked there.
 *
 * @param status: burst status, register 0x51, read at the event.
 * @param cc: received colour code, register 0x52, read at the event.
 */
static void handleBurstEvent(const uint8_t status, const uint8_t cc)
{
    bool ccOk = (cc == configuredCc);

    dmrDebug_log(DMR_DBG_LC, "burst: status %02X cc %u", status, cc);

    switch(status & RX_SYNC_MASK)
    {
        case RX_CLASS_DATA:
            if(ccOk == false)
                return;

            if((status >> 4) == RX_DATA_VOICE_HEADER)
                takeCallLc(cc);
            else if((status >> 4) == RX_DATA_TERMINATOR)
                takeTerminator();
            return;

        case RX_CLASS_VOICE:
            if(ccOk == false)
                return;
            break;

        case RX_CLASS_VOICE_SYNC:
            break;

        default:
            return;
    }

    if(rxState.valid == false)
        return;

    lastBurstTick = getTick();

    if(rxEncrypted == false)
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

    seenFlags  |= status.intFlags;
    seenStatus |= status.rxStatus;

    uint8_t cc = (status.cach >> 4) & 0x0F;

    for(uint8_t flag : INT_FLAG_ORDER)
    {
        if((status.intFlags & flag) == 0)
            continue;

        /* Checked per flag, as handling a terminator ends the call. */
        bool active = rxLocked || rxState.valid;

        if(active && (flag == INT_FLAG_BURST))
            handleBurstEvent(status.rxStatus, cc);
        else if(active && (flag == INT_FLAG_EMBEDDED_LC))
            takeCallLc(cc);

        C6000.clearDmrInterrupts(flag);
    }
}

/**
 * Run the stock firmware's slot handler, once per slot: lock to a transmission
 * on a frame sync, or on a data sync with the configured colour code, keep
 * receiving both slots while locked, and restart the search once the lock is
 * lost or nothing shows up for a while.
 */
static void handleSlot()
{
    uint8_t status = C6000.readCfgRegister(0x51);
    uint8_t cc     = (C6000.readCfgRegister(0x52) >> 4) & 0x0F;
    uint8_t sync   = status & RX_SYNC_MASK;
    bool    ccOk   = (cc == configuredCc);

    if(rxLocked == false)
    {
        if((sync == RX_CLASS_VOICE_SYNC) || ((sync == RX_CLASS_DATA) && ccOk))
        {
            rxLocked = true;
            rxMisses = 0;
            C6000.writeCfgRegister(TX_REG_SLOT_ACTION, RX_SLOT_RECEIVE);
        }
        else if(sync == RX_CLASS_VOICE)
        {
            rxMisses += 1;
            if(rxMisses >= UNLOCKED_MISS_LIMIT)
                armReceiver();
        }

        return;
    }

    bool good = (sync == RX_CLASS_VOICE_SYNC)
                || (((sync == RX_CLASS_VOICE) || (sync == RX_CLASS_DATA))
                    && ccOk);

    if(good)
    {
        rxMisses = 0;
    }
    else
    {
        rxMisses += 1;
        if(rxMisses >= LOCKED_MISS_LIMIT)
        {
            if(rxState.valid)
                endCall("carrier lost");
            else
                armReceiver();

            return;
        }
    }

    C6000.writeCfgRegister(TX_REG_SLOT_ACTION, RX_SLOT_RECEIVE);
}

/*
 * Transmission, as the stock firmware runs it from its timeslot task: every
 * other slot is sent, starting with three voice link control headers, the
 * last one starting the vocoder encode, then voice superframes until the
 * transmission is asked to stop after a frame F, a terminator, an idle slot,
 * and the end.
 */
#define TX_HEADER_REPEATS       2       // Headers before the last one
#define TX_STOP_TIMEOUT         1500    // Longest wind down after unkey, ms

static volatile bool txKeyupRequested;  // Key-up handed to the thread
static uint32_t      txSrcId;           // Own ID, sent as the source
static uint8_t       txHeaders;         // Headers left before the last one
static bool          txLastHeader;      // The last header is still to go
static bool          txAtBoundary;      // The last frame sent was frame F
static bool          txTerminated;      // The terminator has been sent
static bool          txIdleSlot;        // The upcoming slot is the idle one

/**
 * Key up, as the stock firmware does: clear voice, transmission with the
 * chip generating its own slot timing and the receiver off, starting on the
 * next slot. A call being received is dropped; direct mode transmission does
 * not wait for the channel in the stock firmware either.
 */
static void keyUp()
{
    txKeyupRequested = false;
    rxRestoreNeeded  = false;

    if(rxState.valid)
    {
        pthread_mutex_lock(&rxMutex);
        rxState.valid = false;
        pthread_mutex_unlock(&rxMutex);

        rxEncrypted      = false;
        lastPayloadValid = false;
        dmrDebug_log(DMR_DBG_INFO, "call: ended, transmitting");
    }

    dmrAudio_stopRx();

    txHeaders    = TX_HEADER_REPEATS;
    txLastHeader = true;
    txAtBoundary = false;
    txTerminated = false;
    txIdleSlot   = false;
    txSequence   = 0;

    C6000.writeCfgRegister(0x56, C6000.readCfgRegister(0x56) & ~0x10);
    C6000.writeCfgRegister(0x14, txSrcId & 0xFF);
    C6000.writeCfgRegister(0x15, (txSrcId >> 8) & 0xFF);
    C6000.writeCfgRegister(0x16, (txSrcId >> 16) & 0xFF);
    C6000.writeCfgRegister(0x40, 0xA3);
    C6000.writeCfgRegister(TX_REG_SLOT_ACTION, RX_SLOT_RECEIVE);

    rxLocked = false;
    rxMisses = 0;

    /*
     * Capture and encode the microphone from now on, so the first voice
     * frames are ready when the headers are over.
     */
    if(dmrAudio_startTx())
        dmrDebug_log(DMR_DBG_INFO, "audio: microphone encode started");

    dmrDebug_log(DMR_DBG_INFO, "tx: keyed, timing %02X", C6000.getDmrTiming());
}

/**
 * End the transmission, in the slot after the one following the terminator:
 * stop the vocoder encode and the microphone, clear the vocoder buffers and
 * stop transmitting. The receiver comes back on the next event.
 *
 * @param reason: what ended it, for the log.
 */
static void endTransmission(const char *reason)
{
    C6000.writeCfgRegister(0x22, 0x46);
    dmrAudio_stopTx();
    C6000.writeCfgRegister(0x21, 0x03);
    C6000.writeCfgRegister(TX_REG_SLOT_ACTION, RX_SLOT_RECEIVE);

    txState         = TX_IDLE;
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
    if(txIdleSlot)
    {
        C6000.writeCfgRegister(TX_REG_SLOT_ACTION, TX_SLOT_IDLE);
        txIdleSlot = false;
        return;
    }

    txIdleSlot = true;

    if(txTerminated)
    {
        endTransmission("terminator sent");
        return;
    }

    C6000.writeCfgRegister(TX_REG_SLOT_ACTION, TX_SLOT_TRANSMIT);

    if((txHeaders > 0) || txLastHeader)
    {
        C6000.writeDmrLinkControl(txLc);
        C6000.writeCfgRegister(TX_REG_BURST_TYPE, TX_BURST_VOICE_HEADER);

        if(txHeaders > 0)
        {
            txHeaders -= 1;
        }
        else
        {
            txLastHeader = false;
            C6000.writeCfgRegister(0x22, 0x86);     // Start the vocoder encode
        }

        return;
    }

    if(txAtBoundary && txStopRequested)
    {
        C6000.writeDmrLinkControl(txLc);
        C6000.writeCfgRegister(TX_REG_BURST_TYPE, TX_BURST_TERMINATOR);
        txTerminated = true;
        return;
    }

    /*
     * A voice frame: three encoded AMBE frames from the microphone, any not
     * yet available falling back to silence.
     */
    uint8_t payload[VP_LENGTH];
    for(int f = 0; f < VP_LENGTH / DMR_CODEC_FRAME_BYTES; f++)
    {
        uint8_t *slot = &payload[f * DMR_CODEC_FRAME_BYTES];
        if(dmrAudio_popFrame(slot) == false)
            memcpy(slot, TX_SILENCE_AUDIO, DMR_CODEC_FRAME_BYTES);
    }

    dmrPayload_write(payload);
    C6000.writeCfgRegister(TX_REG_BURST_TYPE,
                           TX_BURST_VOICE | (txSequence << 4));

    txAtBoundary = (txSequence == (TX_SUPERFRAME_FRAMES - 1));
    txSequence   = (txSequence + 1) % TX_SUPERFRAME_FRAMES;
}

/**
 * Acknowledge the chip's interrupt flags while transmitting, when they carry
 * nothing to act on.
 */
static void acknowledgeFlags()
{
    uint8_t flags = C6000.readCfgRegister(0x82);
    if(flags != 0)
        C6000.clearDmrInterrupts(flags);
}

/**
 * Bring the receiver back after a transmission, as the stock firmware does:
 * clear the vocoder buffers, reset its decode, then restart the search for a
 * transmission.
 */
static void restoreAfterTx()
{
    rxRestoreNeeded = false;

    C6000.writeCfgRegister(0x20, 0x00);
    C6000.writeCfgRegister(0x21, 0x02);
    C6000.writeCfgRegister(0x22, 0x16);
    C6000.writeCfgRegister(0x22, 0x46);
    armReceiver();
    acknowledgeFlags();
    lastBurstTick = getTick();

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
    (void) arg;
    long long slotDue = -1;

    while(rxThreadRun)
    {
        uint32_t events = (slotDue < 0) ? dmrIrq_waitEvents()
                                        : dmrIrq_takeEvents();
        if(rxThreadRun == false)
            break;

        if(txKeyupRequested)
        {
            keyUp();
            slotDue = -1;
            continue;
        }

        if(rxRestoreNeeded)
        {
            restoreAfterTx();
            slotDue = -1;
            continue;
        }

        if(dmr_txActive())
        {
            slotDue = -1;

            if((events & DMR_IRQ_EVENT_SYSTEM) != 0)
                acknowledgeFlags();

            if((events & DMR_IRQ_EVENT_TIMESLOT) != 0)
                txSlot();

            /*
             * The transmission winds down on its own once unkeyed, within a
             * superframe or so; if the slot clock stopped it would not.
             */
            if(dmr_txActive() && txStopRequested
               && ((getTick() - txStopTick) > TX_STOP_TIMEOUT))
                endTransmission("timed out after unkey");

            continue;
        }

        if((events & DMR_IRQ_EVENT_SYSTEM) != 0)
            handleSystemEvent();

        if((events & DMR_IRQ_EVENT_TIMESLOT) != 0)
            slotDue = getTick() + SLOT_READ_DELAY;

        if((slotDue >= 0) && (getTick() >= slotDue))
        {
            handleSlot();
            slotDue = -1;
        }

        /*
         * A call whose bursts stopped without a terminator and without the
         * lock being dropped, which the slot handler normally catches first.
         */
        if(rxState.valid && ((getTick() - lastBurstTick) > RX_HOLD_TIMEOUT))
            endCall("timeout");

        if(slotDue >= 0)
            sleepFor(0u, 1u);
    }

    return NULL;
}

#else /* DMR_THREAD */

/**
 * Read out a burst the chip has signalled and, if it passes the filters,
 * extract the identities it carries.
 *
 * @return true if the chip reported something received, false if the event
 * was only the slot clock ticking.
 */
static bool handleBurst()
{
    struct C6000_DmrStatus status;
    C6000.readDmrStatus(&status);
    C6000.clearDmrInterrupts(status.intFlags);

    bool received = (status.intFlags & ~INT_FLAG_SLOT) != 0;

    /*
     * An event that is only the slot clock ticking carries nothing to read: the
     * chip still hands back the previous call's link control and payload, which
     * would bring a call that is over straight back.
     */
    if(received == false)
        return false;

    bool    crcOk = (status.rxStatus & RX_STATUS_CRC_ERROR) == 0;
    uint8_t code  = (status.cach >> 4) & 0x0F;

    /*
     * The burst status read here from the thread arrives too late to describe
     * the burst that raised the frame event, so a copy sampled in the system
     * interrupt is used to classify it. Both are logged side by side, together
     * with the two colour code reads, so the bench can confirm the sampled one
     * is trustworthy: "status" is the thread read of 0x51, "isrbs" the same
     * register sampled at the event.
     */
    uint8_t burstStatus = dmrIrq_getRxBurstStatus();

    dmrDebug_log(DMR_DBG_LC,
                 "burst: flags %02X status %02X isrbs %02X cach %02X sync %02X "
                 "cc %u isrcc %u crc %s", status.intFlags, status.rxStatus,
                 burstStatus, status.cach, status.syncStatus, code,
                 dmrIrq_getRxColorCode(), crcOk ? "ok" : "bad");

    /*
     * Stream every burst's payload for the host-side capture harness, tagged
     * with the interrupt-sampled status byte so the host can pick out the voice
     * frames from it: the data type or voice sequence number is in the high
     * nibble, the sync class in the low bits. The capture cannot be gated on
     * the CRC check, which every voice burst fails for lack of fresh link
     * control. Classifying voice on the radio is deferred until the exact rule
     * is confirmed against a full capture; gating on the sync class alone
     * wrongly dropped almost every burst.
     */
    if(dmrDebug_channelEnabled(DMR_DBG_VP) || dmrAudio_running())
    {
        uint8_t payload[VP_LENGTH];
        dmrPayload_read(payload);

        if(dmrDebug_channelEnabled(DMR_DBG_VP))
        {
            uint8_t record[1 + VP_LENGTH];
            record[0] = burstStatus;
            memcpy(&record[1], payload, VP_LENGTH);
            dmrDebug_record(DMR_DBG_VP, "VP", record, sizeof(record));
        }

        /*
         * Feed the three AMBE frames of a voice burst to playback, but only the
         * ones that pass the voice check: in direct mode the receiver hears both
         * timeslots, and the empty one decodes to noise. Gating on each frame's
         * Golay error count keeps the call's slot and drops the other.
         */
        bool repeated = lastPayloadValid
                        && (memcmp(payload, lastPayload, VP_LENGTH) == 0);
        memcpy(lastPayload, payload, VP_LENGTH);
        lastPayloadValid = true;

        if(dmrAudio_running() && (repeated == false))
        {
            for(int f = 0; f < 3; f++)
            {
                const uint8_t *frame = &payload[f * DMR_CODEC_FRAME_BYTES];
                if(dmr_codec_frameIsVoice(frame))
                    dmrAudio_pushFrame(frame);
            }
        }
    }

    if(crcOk == false)
        return received;

    if((status.rxStatus & RX_STATUS_PRIVACY) != 0)
    {
        dmrDebug_log(DMR_DBG_LC, "burst: rejected, encrypted");
        return received;
    }

    /*
     * Colour code is not filtered: on the bench register 0x52 read back the
     * same value whether read here or in the interrupt (0 for a signal that
     * should have been CC1, with occasional 8 and 2), so it does not carry a
     * usable colour code in direct mode - it is the CACH register, and direct
     * mode has no CACH. The interrupt-sampled value is still read and shown
     * for diagnosis, but filtering on it would reject every call. Getting a
     * real direct-mode colour code needs the embedded signalling decoded,
     * which is a separate task.
     */
    uint8_t rxCc = dmrIrq_getRxColorCode();

    uint8_t lc[C6000_LC_LENGTH];
    C6000.readDmrLinkControl(lc);
    dmrDebug_record(DMR_DBG_LC, "LC", lc, sizeof(lc));

    if((lc[0] != LC_FLCO_GROUP) && (lc[0] != LC_FLCO_PRIVATE))
    {
        dmrDebug_log(DMR_DBG_LC, "burst: rejected, unhandled FLCO %02X", lc[0]);
        return received;
    }

    if(memcmp(lc, prevLc, sizeof(lc)) != 0)
    {
        memcpy(prevLc, lc, sizeof(lc));
        lcMatches = 1;
        return received;
    }

    /*
     * Confirmed once the same link control has been seen enough times. The
     * counter is held at the threshold afterwards: it used to grow with every
     * burst and, being a byte, wrapped back through zero during a long call,
     * dropping the receive state for a burst or two each time round.
     */
    if(lcMatches < LC_CONFIRM_COUNT)
    {
        lcMatches += 1;
        if(lcMatches < LC_CONFIRM_COUNT)
            return received;
    }

    uint32_t dstId = (lc[3] << 16) | (lc[4] << 8) | lc[5];
    uint32_t srcId = (lc[6] << 16) | (lc[7] << 8) | lc[8];
    bool     group = (lc[0] == LC_FLCO_GROUP);

    /*
     * Only the start of a call is announced, not every burst of it: the tap
     * link is far slower than the burst rate and logging each one floods it,
     * which stalls the thread doing the logging.
     */
    bool newCall = (rxState.valid == false) || (rxState.srcId != srcId) ||
                   (rxState.dstId != dstId);

    rxState.dstId     = dstId;
    rxState.srcId     = srcId;
    rxState.colorCode = rxCc;
    rxState.groupCall = group;
    rxState.valid     = true;

    if(newCall)
    {
        dmrDebug_log(DMR_DBG_INFO, "call: from %lu to %s %lu, color code %u",
                     (unsigned long) srcId, group ? "TG" : "ID",
                     (unsigned long) dstId, rxCc);

        /* Start voice playback, if the vocoder is installed. */
        if(dmrAudio_startRx())
            dmrDebug_log(DMR_DBG_INFO, "audio: voice playback started");
    }

    return received;
}

#endif /* DMR_THREAD */

bool dmr_txActive()
{
    return txState != TX_IDLE;
}

void dmr_txTimeslotIsr()
{
    switch(txState)
    {
        /*
         * The three link control headers go out on the transmit slots, with an
         * idle slot in between each. The link control is handed to the chip
         * once, on the first header; it holds onto it for the following two.
         */
        case TX_START_1:
            C6000.writeDmrLinkControlFromIsr(txLc);
            C6000.writeCfgRegisterFromIsr(TX_REG_SLOT_ACTION, TX_SLOT_TRANSMIT);
            C6000.writeCfgRegisterFromIsr(TX_REG_BURST_TYPE, TX_BURST_VOICE_HEADER);
            txState = TX_START_2;
            break;

        case TX_START_2:
            C6000.writeCfgRegisterFromIsr(TX_REG_SLOT_ACTION, TX_SLOT_IDLE);
            txState = TX_START_3;
            break;

        case TX_START_3:
            C6000.writeCfgRegisterFromIsr(TX_REG_SLOT_ACTION, TX_SLOT_TRANSMIT);
            C6000.writeCfgRegisterFromIsr(TX_REG_BURST_TYPE, TX_BURST_VOICE_HEADER);
            txState = TX_START_4;
            break;

        case TX_START_4:
            C6000.writeCfgRegisterFromIsr(TX_REG_SLOT_ACTION, TX_SLOT_IDLE);
            txState = TX_START_5;
            break;

        case TX_START_5:
            C6000.writeCfgRegisterFromIsr(TX_REG_SLOT_ACTION, TX_SLOT_TRANSMIT);
            C6000.writeCfgRegisterFromIsr(TX_REG_BURST_TYPE, TX_BURST_VOICE_HEADER);
            txState = TX_GAP;
            break;

        /*
         * The idle slot preceding each voice frame. Leaving the transmission at
         * a superframe boundary, when the next frame would be the first of a
         * superframe, keeps the terminator on the correct slot.
         */
        case TX_GAP:
            C6000.writeCfgRegisterFromIsr(TX_REG_SLOT_ACTION, TX_SLOT_IDLE);
            if(txStopRequested && (txSequence == 0))
                txState = TX_END_1;
            else
                txState = TX_VOICE;
            break;

        /*
         * A voice frame: load the payload on the second bus, then set the slot
         * to transmit it with the current sequence number in the burst type high
         * nibble, and advance the sequence. The payload is three encoded AMBE
         * frames from the microphone; any not yet available (start-up, underrun,
         * or no vocoder installed) fall back to silence, so the call is always
         * on-air even before audio flows.
         */
        case TX_VOICE:
        {
            uint8_t payload[VP_LENGTH];
            for(int f = 0; f < VP_LENGTH / DMR_CODEC_FRAME_BYTES; f++)
            {
                uint8_t *slot = &payload[f * DMR_CODEC_FRAME_BYTES];
                if(dmrAudio_popFrame(slot) == false)
                    memcpy(slot, TX_SILENCE_AUDIO, DMR_CODEC_FRAME_BYTES);
            }
            dmrPayload_write(payload);
            C6000.writeCfgRegisterFromIsr(TX_REG_SLOT_ACTION, TX_SLOT_TRANSMIT);
            C6000.writeCfgRegisterFromIsr(TX_REG_BURST_TYPE,
                                          TX_BURST_VOICE | (txSequence << 4));
            txSequence = (txSequence + 1) % TX_SUPERFRAME_FRAMES;
            txState = TX_GAP;
            break;
        }

        /*
         * The terminator carries the link control again so a late listener can
         * still identify the call as it ends.
         */
        case TX_END_1:
            C6000.writeDmrLinkControlFromIsr(txLc);
            dmrPayload_write(TX_SILENCE_AUDIO);
            C6000.writeCfgRegisterFromIsr(TX_REG_SLOT_ACTION, TX_SLOT_TRANSMIT);
            C6000.writeCfgRegisterFromIsr(TX_REG_BURST_TYPE, TX_BURST_TERMINATOR);
            txState = TX_END_2;
            break;

        /*
         * This interrupt marks the start of the slot carrying the terminator,
         * which must play out untouched: rewriting the timing register here
         * cut the burst short - the far end never saw the call end - and left
         * the chip wedged mid-transmission. Only queue an idle slot and wait
         * out the burst; the reference firmware holds off the teardown for a
         * slot time at this exact point for the same reason.
         */
        case TX_END_2:
            C6000.writeCfgRegisterFromIsr(TX_REG_SLOT_ACTION, TX_SLOT_IDLE);
            txState = TX_END_3;
            break;

        /*
         * The terminator has been sent, so put the slot timing back to
         * reception and stop driving the slot. Going idle here is what the
         * transmitting thread waits on to hand back to the receiver; the full
         * receive bring-up, which needs more than an idle slot action, is left
         * to the thread and flagged here.
         */
        case TX_END_3:
            C6000.writeCfgRegisterFromIsr(0x40,
                static_cast< uint8_t >(C6000_DmrTiming::RX_IDLE));
            C6000.writeCfgRegisterFromIsr(TX_REG_SLOT_ACTION, TX_SLOT_IDLE);
            rxRestoreNeeded = true;
            txState = TX_IDLE;
            break;

        default:
            break;
    }
}

void dmr_startTx(const bool groupCall, const uint32_t dstId,
                 const uint32_t srcId)
{
#if defined(DMR_THREAD)
    /*
     * The key-up itself is left to the DMR thread, which runs the whole
     * transmission, so that it never interleaves with reception.
     */
    dmr_buildVoiceLc(txLc, groupCall, dstId, srcId);
    txSrcId          = srcId;
    txStopRequested  = false;
    txState          = TX_START_1;

    struct dmrIrqCounters counters;
    dmrIrq_getCounters(&counters);
    txReportedTs = counters.timeslot;
    txReportedTx = counters.transmit;
    txReportTick = getTick();

    txKeyupRequested = true;
    dmrIrq_wakeWaiter();

    dmrDebug_log(DMR_DBG_INFO, "tx: keying, to %s %lu from %lu",
                 groupCall ? "TG" : "ID", (unsigned long) dstId,
                 (unsigned long) srcId);
#else
    /*
     * Everything the transmit setup touches on the control bus is fenced off
     * from the interrupt for the duration: the state and link control are set
     * up, the vocoder pipeline started and the slot clock switched to active
     * timing, which is what makes the chip raise the timeslot interrupt that
     * drives the state machine. Only once the bus is released does the next
     * interrupt find a valid state to act on.
     */
    dmrIrq_setBusBusy(true);

    dmr_buildVoiceLc(txLc, groupCall, dstId, srcId);
    txSequence      = 0;
    txStopRequested = false;
    txState         = TX_START_1;

    struct dmrIrqCounters counters;
    dmrIrq_getCounters(&counters);
    txReportedTs = counters.timeslot;
    txReportedTx = counters.transmit;
    txReportTick = getTick();

    C6000.writeCfgRegister(0x21, 0xA2);     // Polite to color code, reset vocoder
    C6000.writeCfgRegister(0x22, 0x86);     // Start vocoder encode, I2S mode
    C6000.setDmrTiming(C6000_DmrTiming::ACTIVE);    // 0x40 = 0xE3, active timing

    /*
     * Read back the registers that shape the transmit modulation, so a signal
     * that goes out as bare carrier can be told apart from wrong deviation or a
     * misrouted modulator. Done while the bus is still fenced off the interrupt.
     * Some of these may be write-only and read back as zero regardless.
     */
    dmrDebug_log(DMR_DBG_INFO,
                 "tx regs: 01=%02X 10=%02X 40=%02X 45=%02X 46=%02X 49=%02X "
                 "4A=%02X 60=%02X E0=%02X",
                 C6000.readCfgRegister(0x01), C6000.readCfgRegister(0x10),
                 C6000.readCfgRegister(0x40), C6000.readCfgRegister(0x45),
                 C6000.readCfgRegister(0x46), C6000.readCfgRegister(0x49),
                 C6000.readCfgRegister(0x4A), C6000.readCfgRegister(0x60),
                 C6000.readCfgRegister(0xE0));

    dmrIrq_setBusBusy(false);

    /*
     * Start capturing and encoding the microphone. Stop any receive playback
     * first: the two share the vocoder state and the radio is never receiving
     * and transmitting at once. Without a vocoder installed this does nothing
     * and the transmit slot machine sends silence, as before.
     */
    dmrAudio_stopRx();
    if(dmrAudio_startTx())
        dmrDebug_log(DMR_DBG_INFO, "audio: microphone encode started");

    dmrDebug_log(DMR_DBG_INFO, "tx: keyed, to %s %lu from %lu",
                 groupCall ? "TG" : "ID", (unsigned long) dstId,
                 (unsigned long) srcId);
#endif
}

void dmr_stopTx()
{
    if(txStopRequested == false)
        txStopTick = getTick();

    txStopRequested = true;
    dmrDebug_log(DMR_DBG_INFO, "tx: unkey requested");
}

void dmr_txTask()
{
    /*
     * The storm guard shuts the interrupt lines down when too many events pile
     * up between two feeds. During a transmission the timeslot interrupt runs
     * the slot clock at its normal rate, which is well within the limit, but
     * the receive poll that usually feeds the guard is not running, so it is
     * fed from here instead.
     */
    dmrIrq_feedWatchdog();

    long long now = getTick();

#if defined(DMR_THREAD)
    /* Let the thread check its wind down even if the slot clock stopped. */
    if(dmr_txActive() && txStopRequested
       && ((now - txStopTick) > TX_STOP_TIMEOUT))
        dmrIrq_wakeWaiter();
#endif

    if((now - txReportTick) < TX_REPORT_INTERVAL)
        return;

    /*
     * Report the slot clock and the state machine's progress while sending. A
     * timeslot rate around 33Hz says the chip is driving the slots and the
     * state cycling through the gap and voice steps says the bursts are being
     * set up; either sitting still is the failure signature.
     */
    struct dmrIrqCounters counters;
    dmrIrq_getCounters(&counters);

    dmrDebug_log(DMR_DBG_IRQ, "tx: state %u seq %u, timeslot %lu transmit %lu "
                 "in %lums", txState, txSequence,
                 (unsigned long) (counters.timeslot - txReportedTs),
                 (unsigned long) (counters.transmit - txReportedTx),
                 (unsigned long) (now - txReportTick));

    txReportedTs = counters.timeslot;
    txReportedTx = counters.transmit;
    txReportTick = now;
}

void dmr_init(const uint8_t colorCode)
{
    memset(&rxState, 0, sizeof(rxState));
    memset(prevLc, 0, sizeof(prevLc));
    lastPayloadValid = false;

    txState         = TX_IDLE;
    txStopRequested = false;
    rxRestoreNeeded = false;

    configuredCc  = colorCode & 0x0F;
    lcMatches     = 0;
    lastSysCount  = 0;
    lastTsCount   = 0;
    lastBurstTick = 0;
    reportedTs   = 0;
    reportedSys  = 0;
    reportedTx   = 0;
    reportTick   = getTick();

    /*
     * Logged in stages: if the radio does not come back, the last line which
     * made it out says which of the two halves is at fault.
     */
    dmrDebug_mark("dmr: configuring baseband");

    /* The voice payload rides on its own SPI bus, brought up here. */
    dmrPayload_init();

    /*
     * The receiver idles in passive timing so the chip can lock to the slot
     * clock of whoever transmits. Active timing made it free run instead:
     * the timeslot line ticked at a steady 33Hz while a transmission on
     * frequency produced no system events at all.
     */
    C6000.setDmrColorCode(configuredCc);
    C6000.setDmrTiming(C6000_DmrTiming::RX_IDLE);

#if defined(DMR_THREAD)
    armReceiver();
    rxEncrypted = false;
#else
    /*
     * Restarting the slot state machine takes the reset twice before asking
     * for a receive, anything less leaves the chip ignoring the next slot.
     */
    C6000.setDmrSlotAction(C6000_DmrSlotAction::RESET);
    C6000.setDmrSlotAction(C6000_DmrSlotAction::IDLE);
    C6000.setDmrSlotAction(C6000_DmrSlotAction::RESET);
    C6000.setDmrSlotAction(C6000_DmrSlotAction::RECEIVE);
#endif

    dmrDebug_mark("dmr: baseband configured");
    checkInitRegisters();

    dmrDebug_mark("dmr: enabling interrupt lines");
    dmrIrq_init();

#if defined(DMR_THREAD)
    rxThreadRun = true;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, RX_THREAD_STKSIZE);
    struct sched_param param;
    param.sched_priority = THREAD_PRIO_RT;
    pthread_attr_setschedparam(&attr, &param);

    if(pthread_create(&rxThread, &attr, rxThreadFunc, NULL) != 0)
    {
        rxThreadRun = false;
        dmrDebug_log(DMR_DBG_INFO, "dmr: reception thread not started");
    }
#endif

    dmrDebug_mark("dmr: receiver running");
}

void dmr_terminate()
{
#if defined(DMR_THREAD)
    if(rxThreadRun)
    {
        rxThreadRun = false;
        dmrIrq_wakeWaiter();
        pthread_join(rxThread, NULL);
    }
#endif

    dmrAudio_stopRx();
    dmrAudio_stopTx();
    dmrIrq_terminate();

    txState         = TX_IDLE;
    txStopRequested = false;
    rxRestoreNeeded = false;

    C6000.setDmrSlotAction(C6000_DmrSlotAction::IDLE);
    dmrPayload_terminate();
    memset(&rxState, 0, sizeof(rxState));

    dmrDebug_log(DMR_DBG_INFO, "dmr: receiver stopped");
}

void dmr_setColorCode(const uint8_t colorCode)
{
    if((colorCode & 0x0F) == configuredCc)
        return;

    configuredCc = colorCode & 0x0F;
    C6000.setDmrColorCode(configuredCc);

    memset(prevLc, 0, sizeof(prevLc));
    lcMatches = 0;

    dmrDebug_log(DMR_DBG_INFO, "dmr: color code set to %u", configuredCc);
}

void dmr_task(struct dmrRxState *rx)
{
#if defined(DMR_THREAD)
    /*
     * Reception runs on its own thread: this hands its state out, keeps the
     * diagnostics going and wakes the thread for the work it would otherwise
     * only notice on its next event, as the slot clock may have stopped:
     * bringing the receiver back after a transmission, or ending a call whose
     * bursts are gone.
     */
    if(gpio_readPin(DMR_TS_INT)  == 0) seenLinesLow |= 0x01;
    if(gpio_readPin(DMR_SYS_INT) == 0) seenLinesLow |= 0x02;
    if(gpio_readPin(DMR_TX_INT)  == 0) seenLinesLow |= 0x04;

    if(rxRestoreNeeded
       || (rxState.valid && ((getTick() - lastBurstTick) > RX_HOLD_TIMEOUT)))
        dmrIrq_wakeWaiter();

    reportIrqActivity();

    pthread_mutex_lock(&rxMutex);
    *rx = rxState;
    pthread_mutex_unlock(&rxMutex);
#else
    struct dmrIrqCounters counters;
    dmrIrq_getCounters(&counters);

    /*
     * A RECEIVE request only covers a single slot: the chip receives one and
     * falls idle unless a fresh request is queued before the next boundary.
     * OpenGD77 rewrites the slot action from the timeslot interrupt every
     * 30ms; the interrupt cannot touch the bitbanged SPI here, so it is done
     * on the slot boundary detected through the timeslot counter instead. The
     * caller polls fast enough (see OpMode_DMR) to hit the window. Receiving
     * on both slots picks up noise on the unused one, but that noise fails the
     * colour code and CRC filters, so it costs an ignored burst rather than a
     * false decode.
     */
    /*
     * The chip is reached over the bitbanged SPI here, which the system
     * interrupt handler also uses to read the colour code. Tell the handler to
     * stand off the bus for the duration so the two never drive it at once.
     */
    dmrIrq_setBusBusy(true);

    /*
     * After a transmission the receiver does not come back on the reset dance
     * alone: the transmission leaves the chip's receive path, its frame sync
     * detection above all, needing the whole DMR mode configuration written
     * again. Re-running that configuration by hand - by leaving DMR mode and
     * coming back - was the workaround found on the bench; doing the chip half
     * of it here has the same effect without dropping out of the mode. The
     * colour code, which the mode configuration sets to a fixed value, is put
     * back afterwards, then the usual receive bring-up follows.
     */
    if(rxRestoreNeeded)
    {
        rxRestoreNeeded = false;

        /* The transmission is over; stop capturing the microphone. */
        dmrAudio_stopTx();

        C6000.dmrMode();
        C6000.setDmrColorCode(configuredCc);
        C6000.setDmrTiming(C6000_DmrTiming::RX_IDLE);
        C6000.setDmrSlotAction(C6000_DmrSlotAction::RESET);
        C6000.setDmrSlotAction(C6000_DmrSlotAction::IDLE);
        C6000.setDmrSlotAction(C6000_DmrSlotAction::RESET);
        C6000.setDmrSlotAction(C6000_DmrSlotAction::RECEIVE);

        memset(prevLc, 0, sizeof(prevLc));
        lcMatches     = 0;
        lastBurstTick = getTick();

        /*
         * The timing register is read back so the log says whether the chip
         * actually took the receive configuration: 43 here is a receiver in
         * passive timing, anything else means the restore did not stick.
         */
        dmrDebug_log(DMR_DBG_INFO, "dmr: receiver restarted after tx, "
                     "timing %02X", C6000.getDmrTiming());
    }

    if(lastTsCount != counters.timeslot)
    {
        lastTsCount = counters.timeslot;
        C6000.setDmrSlotAction(C6000_DmrSlotAction::RECEIVE);
    }

    /* Diagnostics for the activity report, see seenFlags. */
    seenFlags  |= C6000.readCfgRegister(0x82);
    seenStatus |= C6000.readCfgRegister(0x51);
    if(gpio_readPin(DMR_TS_INT)  == 0) seenLinesLow |= 0x01;
    if(gpio_readPin(DMR_SYS_INT) == 0) seenLinesLow |= 0x02;
    if(gpio_readPin(DMR_TX_INT)  == 0) seenLinesLow |= 0x04;

    /*
     * The chip signals a frame event on the system interrupt line, but reading
     * the burst out has to happen here: only the colour code is read from the
     * handler, the rest of the chip access cannot be done from an interrupt.
     */
    if(lastSysCount != counters.system)
    {
        uint32_t missed = counters.system - lastSysCount - 1;
        lastSysCount = counters.system;

        /*
         * Only the state of the most recent burst is still in the chip, so
         * several events collapse into a single read. Say so rather than
         * pretending every burst was seen.
         */
        if(missed > 0)
            dmrDebug_log(DMR_DBG_IRQ, "dmr: %lu bursts not read in time",
                         (unsigned long) missed);

        /*
         * A received frame means a DMR carrier is present. This keeps an
         * established call alive across the voice bursts that sit between the
         * link control updates: those bursts carry no fresh CRC-valid link
         * control, so gating the call on link control alone drops it every
         * few hundred milliseconds during a steady transmission. The identity
         * of the call still comes from handleBurst; this only holds it up.
         * Events which are only the slot clock ticking - it keeps running
         * after a call once the chip has locked to it - do not count.
         */
        if(handleBurst())
            lastBurstTick = getTick();
    }

    dmrIrq_setBusBusy(false);

    /*
     * The chip does not say when a call is over: a terminator burst is not
     * guaranteed to arrive, so the receive state is dropped once no burst
     * has passed the filters for a while.
     */
    if(rxState.valid && ((getTick() - lastBurstTick) > RX_HOLD_TIMEOUT))
    {
        rxState.valid = false;
        memset(prevLc, 0, sizeof(prevLc));
        lastPayloadValid = false;
        lcMatches = 0;

        dmrAudio_stopRx();
        dmrDebug_log(DMR_DBG_INFO, "call: ended");
    }

    reportIrqActivity();

    *rx = rxState;
#endif
}

#endif /* CONFIG_DMR */
