/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/delays.h"
#include "core/dmr_debug.h"
#include "drivers/SPI/spi_stm32.h"
#include "peripherals/gpio.h"
#include "peripherals/spi.h"
#include "hwconfig.h"
#include "dmrIrq.h"
#include "dmr.h"
#include "dmr_audio.h"
#include "protocols/DMR/LinkControl.h"
#include <string.h>

#if defined(CONFIG_DMR)

/*
 * Bits of the receive status register, 0x51: the CRC bit is set when the check
 * failed, so a burst is good when it reads back as zero.
 */
#define RX_STATUS_CRC_ERROR     0x04
#define RX_STATUS_PRIVACY       0x08

/* Full link control frames of a group and of a private call */
#define LC_FLCO_GROUP           0x00
#define LC_FLCO_PRIVATE         0x03

/*
 * The voice payload of a burst: three 9-byte AMBE frames the chip hands over
 * on page 0x03 of its second SPI bus, separate from the bitbanged control bus
 * the rest of the driver uses. The read command is the page number with the
 * read flag set, followed by the register offset; the payload comes back after
 * those two command bytes, so the transfer is two bytes longer than the data.
 */
#define VP_LENGTH               27
#define VP_PAGE_READ            0x83
#define VP_PAGE_WRITE           0x03
#define VP_REGISTER             0x00

/*
 * The chip hands back the link control of the previous transmission until it
 * has decoded the current one, so the same content has to show up in more than
 * one burst before it can be trusted. Only the bytes naming the call are
 * compared: the last three carry error correction the chip fills in, and it
 * differs between the burst types which repeat one and the same call.
 */
#define LC_CONFIRM_COUNT        2
#define LC_ID_LENGTH            9

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
 * Window within which two bursts carrying decodable voice must fall for the
 * voice to be believed, in milliseconds. Real voice bursts arrive every 60ms,
 * so the second one opens the gate; the noise bursts which rarely clear the
 * per-frame check land much further apart than this and never pair up.
 */
#define VOICE_CONFIRM_WINDOW    250

/*
 * How long the receive state is held after the last good burst before the
 * call is considered over, in milliseconds. Voice bursts on the received
 * slot are 60ms apart, so this rides out a few corrupted bursts without
 * keeping the call on screen after the carrier is gone.
 */
#define RX_HOLD_TIMEOUT         500

static struct dmrRxState rxState;
static struct dmrConfig  dmrCfg;
static uint8_t   prevLc[C6000_LC_LENGTH];
static uint8_t   lcMatches;
static uint8_t   voiceStreak;   // Bursts in a row carrying decodable voice
static long long lastVoiceTick; // Tick of the last decodable voice burst
static bool      lcDecided;     // The confirmed link control has been acted on
static uint32_t  lastSysCount;
static uint32_t  lastTsCount;
static long long lastBurstTick;
static uint32_t  reportedTs;
static uint32_t  reportedSys;
static uint32_t  reportedTx;
static long long reportTick;

/*
 * A count per sync pattern seen since the last report, which says whether the
 * far end is a repeater and whether a direct mode transmission names a
 * timeslot. A single burst says little, as these registers are read at the very
 * edge of the instant they are valid for, but the shape of a whole transmission
 * is unambiguous.
 */
static uint32_t  rxSyncSeen[4];
static uint32_t  rxSampled;
static uint32_t  reportedUnsampled;

/*
 * The hunt for a direct mode colour code. Neither register which should hold
 * one does - see the note in handleBurst - but the colour code is certainly on
 * air, in the slot type of every data burst and the embedded signalling of the
 * voice bursts, and commercial radios filter on it. So every distinct
 * combination of the status registers is logged once, and only once: keying a
 * far end on one colour code fills the table, and keying it on another then
 * prints exactly the combinations which differ. Any byte tracking the colour
 * code shows up in that difference and nothing else does.
 */
#define CC_HUNT_SLOTS   16

struct ccHuntEntry
{
    uint8_t flags;
    uint8_t status;
    uint8_t cach;
    uint8_t timecode;
    uint8_t rxInfo;
    uint8_t sync;
};

static struct ccHuntEntry ccHunt[CC_HUNT_SLOTS];
static uint8_t ccHuntUsed;

/*
 * A configuration register access asked for over the debug tap, carried out on
 * the next pass of dmr_task, where the bus is already fenced off from the
 * interrupt handlers. Only one is held at a time, which is all somebody typing
 * at a terminal can ask for. A dump is bounded so that a slip of the fingers
 * cannot ask for hundreds of lines down a link this slow.
 */
#define REG_DUMP_MAX    64

static volatile char    regOp;
static volatile uint8_t regAddr;
static volatile uint8_t regValue;

/* Names of the sync patterns, indexed by the low two bits of register 0x5F. */
static const char *const SYNC_NAMES[4] = { "ms", "bs", "ts1", "ts2" };

static volatile uint8_t txState;        // Current transmit slot state
static volatile uint8_t txSequence;     // Voice frame sequence, 0..5

/*
 * How many payload slots carried encoded audio and how many fell back to
 * silence. A perfectly framed but silent call looks identical on the air to one
 * carrying speech, so without these there is no way to tell whether the encoder
 * is keeping up with the fifty frames a second the slot timing demands.
 */
static volatile uint32_t txVoiceFrames;
static volatile uint32_t txStarvedFrames;
static volatile bool    txStopRequested;// Unkey asked, waiting for a boundary
static volatile bool    rxRestoreNeeded;// Receiver bring-up owed after a TX
static uint8_t          txLc[DMR_LC_LENGTH]; // Link control sent in the bursts
static uint32_t  txReportedTs;          // Timeslot count at the last tx report
static uint32_t  txReportedTx;          // Transmit count at the last tx report
static long long txReportTick;          // Tick of the last transmit report

/* Interval between two transmit progress reports, in milliseconds */
#define TX_REPORT_INTERVAL      500

/**
 * Bring up the second SPI bus that carries the voice payload. Unlike the
 * bitbanged control interface this is a hardware peripheral, so it does not
 * contend with the control bus and can be driven straight from the driver.
 * The chip wants clock idle high and data sampled on the falling edge; the
 * bus clock is the APB1 clock divided by eight, well within the chip's limit.
 */
static void payloadBusInit()
{
    gpio_setMode(DMR_V_CLK,  ALTERNATE | ALTERNATE_FUNC(5));
    gpio_setMode(DMR_V_MISO, ALTERNATE | ALTERNATE_FUNC(5));
    gpio_setMode(DMR_V_MOSI, ALTERNATE | ALTERNATE_FUNC(5));
    gpio_setMode(DMR_V_CS,   OUTPUT);
    gpio_setPin(DMR_V_CS);      // Chip select idle high

    spiStm32_init(&payload_spi, 6000000, SPI_FLAG_CPOL);
}

/**
 * Shut the voice payload bus back down and park its lines.
 */
static void payloadBusTerminate()
{
    spiStm32_terminate(&payload_spi);

    gpio_setMode(DMR_V_CLK,  INPUT);
    gpio_setMode(DMR_V_MISO, INPUT);
    gpio_setMode(DMR_V_MOSI, INPUT);
    gpio_setMode(DMR_V_CS,   INPUT);
}

/**
 * Read the raw voice payload of the burst the chip has just decoded. The two
 * command bytes are clocked out first and their returned bytes discarded, the
 * payload follows.
 *
 * @param payload: destination buffer, VP_LENGTH bytes long.
 */
static void payloadRead(uint8_t *payload)
{
    uint8_t tx[2 + VP_LENGTH];
    uint8_t rx[2 + VP_LENGTH];

    memset(tx, 0, sizeof(tx));
    tx[0] = VP_PAGE_READ;
    tx[1] = VP_REGISTER;

    gpio_clearPin(DMR_V_CS);
    spi_transfer(&payload_spi, tx, rx, sizeof(tx));
    gpio_setPin(DMR_V_CS);

    memcpy(payload, &rx[2], VP_LENGTH);
}

/**
 * Load the voice payload the chip transmits in the upcoming slot. The two
 * command bytes select the payload page and its first register, the payload
 * follows in the same transfer. This runs from the timeslot interrupt: the
 * payload rides on the second SPI bus, a hardware peripheral separate from the
 * bitbanged control bus, so it is safe to drive from an interrupt with no bus
 * coordination.
 *
 * @param payload: source buffer, VP_LENGTH bytes long.
 */
static void payloadWrite(const uint8_t *payload)
{
    uint8_t tx[2 + VP_LENGTH];

    tx[0] = VP_PAGE_WRITE;
    tx[1] = VP_REGISTER;
    memcpy(&tx[2], payload, VP_LENGTH);

    gpio_clearPin(DMR_V_CS);
    spi_transfer(&payload_spi, tx, NULL, sizeof(tx));
    gpio_setPin(DMR_V_CS);
}

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
                 dmrCfg.colorCode);
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

    dmrDebug_log(DMR_DBG_IRQ, "irq: timeslot %lu system %lu transmit %lu in %lums",
                 (unsigned long) (counters.timeslot - reportedTs),
                 (unsigned long) (counters.system   - reportedSys),
                 (unsigned long) (counters.transmit - reportedTx),
                 (unsigned long) (now - reportTick));

    /*
     * What the received bursts carried, on the interrupt channel as a
     * diagnostic rather than in the default output. The sync counts came back
     * mobile station sourced throughout on the bench, which is what says a
     * direct mode transmission does not name its timeslot. The unsampled count
     * is the caveat on the line: those events left the snapshot describing an
     * older burst.
     */
    if(rxSampled > 0)
    {
        uint32_t unsampled = dmrIrq_getRxUnsampled();

        dmrDebug_log(DMR_DBG_IRQ, "rx: %lu bursts, sync "
                     "ms/bs/ts1/ts2 %lu/%lu/%lu/%lu, %lu unsampled",
                     (unsigned long) rxSampled,
                     (unsigned long) rxSyncSeen[DMR_SYNC_MS],
                     (unsigned long) rxSyncSeen[DMR_SYNC_BS],
                     (unsigned long) rxSyncSeen[DMR_SYNC_TS1],
                     (unsigned long) rxSyncSeen[DMR_SYNC_TS2],
                     (unsigned long) (unsampled - reportedUnsampled));

        reportedUnsampled = unsampled;
        rxSampled = 0;
        memset(rxSyncSeen, 0, sizeof(rxSyncSeen));
    }

    reportedTs  = counters.timeslot;
    reportedSys = counters.system;
    reportedTx  = counters.transmit;
    reportTick  = now;
}

/**
 * Note the register combination a burst arrived with, and say so the first time
 * each one appears. Bounded by the table, so a long call cannot flood the tap.
 *
 * @param isr: registers sampled at the frame event.
 */
static void ccHuntRecord(const struct dmrIrqRxSnapshot *isr)
{
    struct ccHuntEntry entry;
    entry.flags    = isr->intFlags;
    entry.status   = isr->status;
    entry.cach     = isr->cach;
    entry.timecode = isr->timecode;
    entry.rxInfo   = isr->rxInfo;
    entry.sync     = isr->sync;

    for(uint8_t i = 0; i < ccHuntUsed; i++)
    {
        if(memcmp(&ccHunt[i], &entry, sizeof(entry)) == 0)
            return;
    }

    if(ccHuntUsed >= CC_HUNT_SLOTS)
    {
        /* Said once per fill, or a full table reads as "nothing changed". */
        if(ccHuntUsed == CC_HUNT_SLOTS)
        {
            ccHuntUsed += 1;
            dmrDebug_log(DMR_DBG_INFO, "hunt: table full");
        }
        return;
    }

    ccHunt[ccHuntUsed] = entry;
    ccHuntUsed += 1;

    dmrDebug_log(DMR_DBG_INFO, "hunt: flags %02X status %02X cach %02X "
                 "t57 %02X r90 %02X sync %02X", entry.flags, entry.status,
                 entry.cach, entry.timecode, entry.rxInfo, entry.sync);
}

/**
 * Watch the CACH register for the colour code of the station being received.
 *
 * The chip delivers a colour code only for transmissions carrying a CACH -
 * hotspots and repeaters - as sparse frame events reading (CC << 4) | 0xF;
 * a mobile station transmitting direct carries its colour code only in
 * signalling the chip consumes without exposing, so from such a station no
 * observation ever arrives and the code stays unknown. Bench-established
 * against an openSPOT: the register tracked its colour code from 5 to 3,
 * while a subscriber radio on colour code 1 never produced a reading.
 *
 * Observations are rare - the bench saw about one per call - so the first
 * nonzero one is taken as it stands: demanding a second reading which may
 * never come held the code back entirely. The only wrong readings ever
 * seen carried a zero code, which is never taken at all; every nonzero
 * reading so far has matched the far end.
 *
 * @param cach: CACH register sampled at the frame event.
 */
static void observeColorCode(const uint8_t cach)
{
    if((cach & 0x0F) != 0x0F)
        return;

    uint8_t code = (cach >> 4) & 0x0F;
    if(code == 0)
        return;

    if(rxState.colorCode != code)
    {
        rxState.colorCode = code;
        dmrDebug_log(DMR_DBG_INFO, "call: colour code %u observed", code);
    }
}

/**
 * Decide whether a call is meant for this radio. A group call has to be to the
 * talkgroup the radio is set to and a private call to its own DMR ID, which is
 * how a radio hears somebody calling it directly whatever talkgroup it sits on.
 * The promiscuous setting takes everything the channel carries instead, which
 * is what a monitoring receiver wants and what the radio does by default while
 * DMR is being brought up.
 *
 * @param groupCall: true for a group call, false for a private one.
 * @param dstId: destination the call is addressed to.
 * @return true if the call is to be received.
 */
static bool callAccepted(const bool groupCall, const uint32_t dstId)
{
    if(dmrCfg.rxPromiscuous)
        return true;

    if(groupCall)
        return dstId == dmrCfg.talkgroup;

    return (dmrCfg.dmrId != 0) && (dstId == dmrCfg.dmrId);
}

/**
 * Read out a burst the chip has signalled and, if it passes the filters,
 * extract the identities it carries.
 */
static void handleBurst()
{
    struct C6000_DmrStatus status;
    C6000.readDmrStatus(&status);
    C6000.clearDmrInterrupts(status.intFlags);

    bool    crcOk = (status.rxStatus & RX_STATUS_CRC_ERROR) == 0;

    /*
     * The registers read here from the thread arrive too late to describe the
     * burst which raised the frame event, so the copy the system interrupt
     * sampled at the event is what gets used and accumulated. Both sets are
     * logged side by side, in the order flags, status, CACH, sync, so the bench
     * can see for itself how far apart the two readings are.
     */
    struct dmrIrqRxSnapshot isr;
    dmrIrq_getRxSnapshot(&isr);

    uint8_t burstStatus = isr.status;
    uint8_t syncType    = isr.sync & 0x03;
    uint8_t code        = (isr.cach >> 4) & 0x0F;

    if(isr.valid)
    {
        rxSyncSeen[syncType] += 1;
        rxSampled += 1;
        ccHuntRecord(&isr);
        observeColorCode(isr.cach);
    }

    dmrDebug_log(DMR_DBG_LC,
                 "burst: isr %02X/%02X/%02X/%02X thr %02X/%02X/%02X/%02X "
                 "cc %u sync %s crc %s",
                 isr.intFlags, isr.status, isr.cach, isr.sync,
                 status.intFlags, status.rxStatus, status.cach,
                 status.syncStatus, code, SYNC_NAMES[syncType],
                 crcOk ? "ok" : "bad");

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
    if(dmrDebug_channelEnabled(DMR_DBG_VP) || rxState.valid)
    {
        uint8_t payload[VP_LENGTH];
        payloadRead(payload);

        if(dmrDebug_channelEnabled(DMR_DBG_VP))
        {
            uint8_t record[1 + VP_LENGTH];
            record[0] = burstStatus;
            memcpy(&record[1], payload, VP_LENGTH);
            dmrDebug_record(DMR_DBG_VP, "VP", record, sizeof(record));
        }

        /*
         * Classify the payload of every burst of an accepted call by the
         * Golay error count of its frames. This drives two things: the empty
         * slot of a direct mode call decodes to noise and is dropped frame by
         * frame, and the chip refuses to hand over the voice of a call on the
         * wrong colour code while still delivering its link control - so
         * decodable voice arriving is what separates a call being received
         * from one the chip is rejecting, and the receive state records it.
         *
         * A single passing burst is not believed: the noise slot's garbage
         * clears the per-frame check just often enough that a rejected call
         * would blip through, which on the bench was heard as a moment of
         * leaked audio. Two passing bursts within a short window are needed:
         * real voice arrives every 60ms so the second burst opens the gate,
         * while the stray passes land too far apart to ever pair up.
         *
         * A burst with nothing decodable leaves the gate alone. The receiver
         * hears both timeslots and the empty one's noise falls between the
         * real voice bursts, so closing the gate on it cut the audio into
         * rhythmic holes with a pop at each edge; the gate only closes once
         * decodable voice stops arriving altogether.
         *
         * Voice bursts carry no link control of their own, so they can only
         * be attributed to the call the link control last identified:
         * playback therefore follows the receive state, set only for a call
         * which passed the filters below and cleared when one is turned away.
         */
        if(rxState.valid)
        {
            int passed = 0;
            for(int f = 0; f < 3; f++)
            {
                if(dmr_codec_frameIsVoice(&payload[f * DMR_CODEC_FRAME_BYTES]))
                    passed += 1;
            }

            if(passed > 0)
            {
                long long now = getTick();

                if((now - lastVoiceTick) > VOICE_CONFIRM_WINDOW)
                    voiceStreak = 0;

                if(voiceStreak < 2)
                    voiceStreak += 1;

                lastVoiceTick = now;
            }

            if((passed > 0) && (voiceStreak >= 2))
            {
                if(dmrAudio_running())
                {
                    for(int f = 0; f < 3; f++)
                    {
                        const uint8_t *frame = &payload[f * DMR_CODEC_FRAME_BYTES];
                        if(dmr_codec_frameIsVoice(frame))
                            dmrAudio_pushFrame(frame);
                    }
                }
            }
        }
    }

    if(crcOk == false)
        return;

    if((status.rxStatus & RX_STATUS_PRIVACY) != 0)
    {
        dmrDebug_log(DMR_DBG_LC, "burst: rejected, encrypted");
        return;
    }

    /*
     * The receiver is colour code promiscuous, because neither register we have
     * tried carries the colour code of the call. On the bench, against a
     * station transmitting on colour code 1, register 0x52 held zero in its
     * colour code field throughout, with bit 7 toggling burst by burst - the
     * timecode, not a colour code - and the reading did not change when this
     * radio was set to colour code 7 instead, so it is not echoing our own
     * setting either. That register is the CACH, which direct mode does not
     * have. Register 0x1F does not gate reception either: with the two colour
     * codes mismatched the call still decoded, so it is the colour code this
     * radio transmits rather than a receive filter.
     *
     * This is a gap in what we can read out of the chip, NOT an absence on air:
     * direct mode carries the colour code in the slot type of every data burst
     * and in the embedded signalling of voice bursts B to F, and commercial
     * radios filter on it in direct mode perfectly well. Where the chip exposes
     * it - another status register, a raw burst path, or a mode bit which makes
     * its own matcher look at the burst instead of the CACH - is unfinished
     * business, not a settled impossibility.
     */

    uint8_t lc[C6000_LC_LENGTH];
    C6000.readDmrLinkControl(lc);
    dmrDebug_record(DMR_DBG_LC, "LC", lc, sizeof(lc));

    if((lc[0] != LC_FLCO_GROUP) && (lc[0] != LC_FLCO_PRIVATE))
    {
        dmrDebug_log(DMR_DBG_LC, "burst: rejected, unhandled FLCO %02X", lc[0]);
        return;
    }

    /*
     * Only the bytes carrying the identity are compared. The chip fills the
     * last three with error correction which differs between the burst types
     * repeating one and the same call, so comparing the whole frame made a
     * steady call look like a new one every time round - announced again, and
     * playback started again, several times a transmission.
     */
    if(memcmp(lc, prevLc, LC_ID_LENGTH) != 0)
    {
        memcpy(prevLc, lc, LC_ID_LENGTH);
        lcMatches = 1;
        lcDecided = false;
        return;
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
            return;
    }

    uint32_t dstId = (lc[3] << 16) | (lc[4] << 8) | lc[5];
    uint32_t srcId = (lc[6] << 16) | (lc[7] << 8) | lc[8];
    bool     group = (lc[0] == LC_FLCO_GROUP);

    /*
     * Only the start of a call is acted on, not every burst of it: the same
     * link control keeps coming back for as long as the call lasts, and the tap
     * link is far slower than the burst rate, so logging each one floods it and
     * stalls the thread doing the logging.
     */
    if(lcDecided)
        return;

    lcDecided = true;

    /*
     * Turn away a call which is not addressed to us. A call in progress is
     * dropped along with it: playback follows the receive state, so leaving it
     * up would let the rejected call be heard through the tail of the one it
     * interrupted.
     */
    if(callAccepted(group, dstId) == false)
    {
        dmrDebug_log(DMR_DBG_INFO, "call: ignored, from %lu to %s %lu",
                     (unsigned long) srcId, group ? "TG" : "ID",
                     (unsigned long) dstId);

        if(rxState.valid)
        {
            rxState.valid = false;
            dmrAudio_stopRx();
        }

        return;
    }

    rxState.dstId     = dstId;
    rxState.srcId     = srcId;
    rxState.groupCall = group;
    rxState.valid     = true;

    dmrDebug_log(DMR_DBG_INFO, "call: from %lu to %s %lu",
                 (unsigned long) srcId, group ? "TG" : "ID",
                 (unsigned long) dstId);

    /* Start voice playback, if the vocoder is installed. */
    if(dmrAudio_startRx())
        dmrDebug_log(DMR_DBG_INFO, "audio: voice playback started");
}

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
                {
                    memcpy(slot, TX_SILENCE_AUDIO, DMR_CODEC_FRAME_BYTES);
                    txStarvedFrames += 1;
                }
                else
                {
                    txVoiceFrames += 1;
                }
            }
            payloadWrite(payload);
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
            payloadWrite(TX_SILENCE_AUDIO);
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
    txVoiceFrames   = 0;
    txStarvedFrames = 0;

    struct dmrIrqCounters counters;
    dmrIrq_getCounters(&counters);
    txReportedTs = counters.timeslot;
    txReportedTx = counters.transmit;
    txReportTick = getTick();

    C6000.writeCfgRegister(0x21, 0xA2);     // Polite to color code, reset vocoder
    C6000.writeCfgRegister(0x22, 0x86);     // Start vocoder encode, I2S mode
    C6000.setDmrTiming(C6000_DmrTiming::ACTIVE);    // 0x40 = 0xE3, active timing

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
}

void dmr_stopTx()
{
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

void dmr_init(const struct dmrConfig *cfg)
{
    memset(&rxState, 0, sizeof(rxState));
    memset(prevLc, 0, sizeof(prevLc));
    rxState.colorCode = DMR_CC_UNKNOWN;

    txState         = TX_IDLE;
    txStopRequested = false;
    rxRestoreNeeded = false;

    dmrCfg           = *cfg;
    dmrCfg.colorCode = cfg->colorCode & 0x0F;
    lcMatches     = 0;
    lcDecided     = false;
    lastSysCount  = 0;
    lastTsCount   = 0;
    lastBurstTick = 0;
    reportedTs   = 0;
    reportedSys  = 0;
    reportedTx   = 0;
    reportTick   = getTick();

    rxSampled         = 0;
    reportedUnsampled = 0;
    ccHuntUsed        = 0;
    memset(rxSyncSeen, 0, sizeof(rxSyncSeen));
    memset(ccHunt, 0, sizeof(ccHunt));

    /*
     * Logged in stages: if the radio does not come back, the last line which
     * made it out says which of the two halves is at fault.
     */
    dmrDebug_mark("dmr: configuring baseband");

    /* The voice payload rides on its own SPI bus, brought up here. */
    payloadBusInit();

    /*
     * The receiver idles in passive timing so the chip can lock to the slot
     * clock of whoever transmits. Active timing made it free run instead:
     * the timeslot line ticked at a steady 33Hz while a transmission on
     * frequency produced no system events at all.
     */
    C6000.setDmrColorCode(dmrCfg.colorCode);
    C6000.setDmrTiming(C6000_DmrTiming::RX_IDLE);

    /*
     * Restarting the slot state machine takes the reset twice before asking
     * for a receive, anything less leaves the chip ignoring the next slot.
     */
    C6000.setDmrSlotAction(C6000_DmrSlotAction::RESET);
    C6000.setDmrSlotAction(C6000_DmrSlotAction::IDLE);
    C6000.setDmrSlotAction(C6000_DmrSlotAction::RESET);
    C6000.setDmrSlotAction(C6000_DmrSlotAction::RECEIVE);

    dmrDebug_mark("dmr: baseband configured");
    checkInitRegisters();

    dmrDebug_mark("dmr: enabling interrupt lines");
    dmrIrq_init();
    dmrDebug_mark("dmr: receiver running");
}

void dmr_terminate()
{
    dmrAudio_stopRx();
    dmrAudio_stopTx();
    dmrIrq_terminate();

    txState         = TX_IDLE;
    txStopRequested = false;
    rxRestoreNeeded = false;

    C6000.setDmrSlotAction(C6000_DmrSlotAction::IDLE);
    payloadBusTerminate();
    memset(&rxState, 0, sizeof(rxState));

    dmrDebug_log(DMR_DBG_INFO, "dmr: receiver stopped");
}

void dmr_setConfig(const struct dmrConfig *cfg)
{
    uint8_t colorCode = cfg->colorCode & 0x0F;
    bool    ccChanged = (colorCode != dmrCfg.colorCode);

    dmrCfg           = *cfg;
    dmrCfg.colorCode = colorCode;

    /*
     * Only the colour code reaches the chip, and only when it has changed: the
     * write costs a bus transaction and drops the link control being tracked,
     * which would restart every call in progress each time any unrelated
     * setting is touched.
     */
    if(ccChanged == false)
        return;

    /*
     * The data link layer latches the local colour code when its modules are
     * reset, so writing the register alone leaves the old code in force: on
     * the bench a changed code behaved differently before and after a mode
     * cycle, whose configuration path ends with this same reset. Pulse the
     * reset here too, and re-arm the slot state machine afterwards exactly as
     * the bring-up does.
     */
    dmrIrq_setBusBusy(true);
    C6000.setDmrColorCode(dmrCfg.colorCode);
    C6000.writeCfgRegister(0x00, 0x3F);
    C6000.setDmrSlotAction(C6000_DmrSlotAction::RESET);
    C6000.setDmrSlotAction(C6000_DmrSlotAction::IDLE);
    C6000.setDmrSlotAction(C6000_DmrSlotAction::RESET);
    C6000.setDmrSlotAction(C6000_DmrSlotAction::RECEIVE);
    dmrIrq_setBusBusy(false);

    memset(prevLc, 0, sizeof(prevLc));
    lcMatches = 0;
    lcDecided = false;

    dmrDebug_log(DMR_DBG_INFO, "dmr: color code set to %u", dmrCfg.colorCode);
}

/**
 * Carry out a register access asked for over the debug tap. Called from
 * dmr_task with the bus already held.
 */
static void servicePendingRegisterOp()
{
    char    op    = regOp;
    uint8_t addr  = regAddr;
    uint8_t value = regValue;

    if(op == 0)
        return;

    regOp = 0;

    switch(op)
    {
        case 'r':
            dmrDebug_log(DMR_DBG_INFO, "reg: %02X = %02X", addr,
                         C6000.readCfgRegister(addr));
            break;

        case 'w':
            C6000.writeCfgRegister(addr, value);
            dmrDebug_log(DMR_DBG_INFO, "reg: %02X <- %02X, reads back %02X",
                         addr, value, C6000.readCfgRegister(addr));
            break;

        case 'd':
        {
            unsigned int last = value;

            if(last < addr)
                last = addr;

            if((last - addr) >= REG_DUMP_MAX)
                last = addr + REG_DUMP_MAX - 1;

            for(unsigned int a = addr; a <= last; a++)
            {
                dmrDebug_log(DMR_DBG_INFO, "reg: %02X = %02X",
                             a, C6000.readCfgRegister((uint8_t) a));
            }
            break;
        }

        default:
            dmrDebug_log(DMR_DBG_INFO, "reg: unknown operation '%c'", op);
            break;
    }
}

void dmr_debugRegisterOp(char op, uint8_t addr, uint8_t value)
{
    regAddr  = addr;
    regValue = value;
    regOp    = op;
}

void dmr_task(struct dmrRxState *rx)
{
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

    servicePendingRegisterOp();

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
        C6000.setDmrColorCode(dmrCfg.colorCode);
        C6000.setDmrTiming(C6000_DmrTiming::RX_IDLE);
        C6000.setDmrSlotAction(C6000_DmrSlotAction::RESET);
        C6000.setDmrSlotAction(C6000_DmrSlotAction::IDLE);
        C6000.setDmrSlotAction(C6000_DmrSlotAction::RESET);
        C6000.setDmrSlotAction(C6000_DmrSlotAction::RECEIVE);

        memset(prevLc, 0, sizeof(prevLc));
        lcMatches     = 0;
        lcDecided     = false;
        voiceStreak   = 0;
        rxState.colorCode   = DMR_CC_UNKNOWN;
        rxState.voiceActive = false;
        lastBurstTick = getTick();

        dmrDebug_log(DMR_DBG_INFO, "tx: %lu payload slots carried audio, "
                     "%lu fell back to silence",
                     (unsigned long) txVoiceFrames,
                     (unsigned long) txStarvedFrames);
    }

    if(lastTsCount != counters.timeslot)
    {
        lastTsCount = counters.timeslot;
        C6000.setDmrSlotAction(C6000_DmrSlotAction::RECEIVE);
    }

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
         * A frame event means a DMR carrier is present. This keeps an
         * established call alive across the voice bursts that sit between the
         * link control updates: those bursts carry no fresh CRC-valid link
         * control, so gating the call on link control alone drops it every
         * few hundred milliseconds during a steady transmission. The identity
         * of the call still comes from handleBurst; this only holds it up.
         */
        lastBurstTick = getTick();

        handleBurst();
    }

    dmrIrq_setBusBusy(false);

    /*
     * The chip does not say when a call is over: a terminator burst is not
     * guaranteed to arrive, so the receive state is dropped once no burst
     * has passed the filters for a while.
     */
    /*
     * Voice is considered flowing while decodable bursts keep arriving; the
     * window rides out the link control bursts interleaved with the voice,
     * which carry no frames of their own.
     */
    rxState.voiceActive = rxState.valid &&
                          ((getTick() - lastVoiceTick) < RX_HOLD_TIMEOUT);

    if(rxState.valid && ((getTick() - lastBurstTick) > RX_HOLD_TIMEOUT))
    {
        rxState.valid = false;
        memset(prevLc, 0, sizeof(prevLc));
        lcMatches   = 0;
        lcDecided   = false;
        voiceStreak = 0;
        rxState.colorCode   = DMR_CC_UNKNOWN;
        rxState.voiceActive = false;

        dmrAudio_stopRx();
        dmrDebug_log(DMR_DBG_INFO, "call: ended");

        /*
         * Start the register hunt afresh for the next call: the table exists
         * to bound the log rate within a transmission, not to hide what a
         * later call does differently - a register experiment between two
         * keyups is judged by exactly that difference.
         */
        ccHuntUsed = 0;
        memset(ccHunt, 0, sizeof(ccHunt));
    }

    reportIrqActivity();

    *rx = rxState;
}

#endif /* CONFIG_DMR */
