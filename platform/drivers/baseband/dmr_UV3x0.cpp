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
#include "protocols/DMR/Datatypes.h"
#include "protocols/DMR/LinkControl.h"
#include "protocols/DMR/CallState.h"
#include <string.h>

#if defined(CONFIG_DMR)

/*
 * Bits of the receive status register, 0x51: the CRC bit is set when the check
 * failed, so a burst is good when it reads back as zero.
 */
#define RX_STATUS_CRC_ERROR     0x04
#define RX_STATUS_PRIVACY       0x08

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
 * The state of the call being received, one entry per logical slot. Direct
 * mode uses a single slot and nothing on air names one, so only the first
 * entry does anything today; the shape is the protocol's.
 */
static struct dmrCallState rxCall[DMR_SLOT_COUNT];

static struct dmrConfig  dmrCfg;
static uint32_t  lastSysCount;
static uint32_t  lastTsCount;
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
/*
 * Link control sent in the bursts, held as the chip's register image: the
 * nine information octets first, then the parity bytes the chip fills in.
 */
static uint8_t          txLc[C6000_LC_LENGTH];
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
 * reading so far has matched the far end. Distrusting zero is a fact about
 * this chip's misreads, not about the air - colour code 0 is legal - which
 * is why the filter sits here and not in the protocol layer.
 *
 * @param cach: CACH register sampled at the frame event.
 * @return the observed colour code, or DMR_CC_UNKNOWN.
 */
static uint8_t observedColourCode(const uint8_t cach)
{
    if((cach & 0x0F) != 0x0F)
        return DMR_CC_UNKNOWN;

    uint8_t code = (cach >> 4) & 0x0F;
    if(code == 0)
        return DMR_CC_UNKNOWN;

    return code;
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
 * Read out a burst the chip has signalled, describe it as a chip-independent
 * burst for the call state, and act on what comes back: the accept policy,
 * the audio and the log lines are all decided here, where the radio's
 * settings and hardware are at hand.
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

    struct dmrCallState *cs = &rxCall[0];

    /*
     * The chip-independent description of this frame event. An event whose
     * registers went unsampled leaves the type and sync unknown: the call
     * state must see the event for its liveness, but must not mistake stale
     * register bytes for a classification.
     */
    struct dmrBurst burst;
    memset(&burst, 0, sizeof(burst));
    burst.slot       = 0;
    burst.dataType   = DMR_DATA_UNKNOWN;
    burst.syncClass  = DMR_SYNC_UNKNOWN;
    burst.colourCode = DMR_CC_UNKNOWN;
    burst.crcOk      = crcOk;
    burst.privacy    = (status.rxStatus & RX_STATUS_PRIVACY) != 0;

    if(isr.valid)
    {
        burst.dataType   = (isr.status >> 4) & 0x0F;
        burst.syncClass  = syncType;
        burst.colourCode = observedColourCode(isr.cach);

        rxSyncSeen[syncType] += 1;
        rxSampled += 1;

        if((burst.colourCode != DMR_CC_UNKNOWN)
           && (burst.colourCode != cs->call.colourCode))
        {
            dmrDebug_log(DMR_DBG_INFO, "call: colour code %u observed",
                         burst.colourCode);
        }
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
     * control.
     *
     * The classification of an accepted call's payload rides on the burst as
     * the count of frames passing the vocoder's Golay check: the voice gate
     * in the call state feeds on the count, and the count alone, so the
     * protocol layer never touches the codec. The gate below is judged on
     * the call as it stood when the payload was read: the same burst's link
     * control may just have asked for a decision, and that decision is taken
     * further down, after the audio of the call it belongs to.
     */
    bool wasValid = cs->call.valid;

    if(dmrDebug_channelEnabled(DMR_DBG_VP) || wasValid)
    {
        payloadRead(burst.payload);

        if(dmrDebug_channelEnabled(DMR_DBG_VP))
        {
            uint8_t record[1 + VP_LENGTH];
            record[0] = burstStatus;
            memcpy(&record[1], burst.payload, VP_LENGTH);
            dmrDebug_record(DMR_DBG_VP, "VP", record, sizeof(record));
        }

        if(wasValid)
        {
            for(int f = 0; f < 3; f++)
            {
                const uint8_t *frame = &burst.payload[f * DMR_CODEC_FRAME_BYTES];
                if(dmr_codec_frameIsVoice(frame))
                    burst.voiceFrames += 1;
            }
        }
    }

    /*
     * The link control, when this burst can carry one: it is only read for a
     * burst whose own checks passed, and an encrypted call is turned away
     * here, before its payload can reach the vocoder. The chip's image
     * carries the identity in its first nine bytes and error correction of
     * its own making in the rest, so only the identity crosses over.
     */
    if(crcOk && (burst.privacy == false))
    {
        uint8_t lc[C6000_LC_LENGTH];
        C6000.readDmrLinkControl(lc);
        dmrDebug_record(DMR_DBG_LC, "LC", lc, sizeof(lc));

        memcpy(burst.lc, lc, DMR_LC_LENGTH);
        burst.lcValid = true;

        if((burst.lc[0] != DMR_FLCO_GROUP) && (burst.lc[0] != DMR_FLCO_PRIVATE))
        {
            dmrDebug_log(DMR_DBG_LC, "burst: rejected, unhandled FLCO %02X",
                         burst.lc[0]);
        }
    }
    else if(crcOk && burst.privacy)
    {
        dmrDebug_log(DMR_DBG_LC, "burst: rejected, encrypted");
    }

    uint8_t events = dmr_callState_feedBurst(cs, &burst, getTick());

    /* Play the voice of this burst, once the call state's gate is open. */
    if(wasValid && (burst.voiceFrames > 0) && dmr_callState_voiceGateOpen(cs))
    {
        if(dmrAudio_running())
        {
            for(int f = 0; f < 3; f++)
            {
                const uint8_t *frame = &burst.payload[f * DMR_CODEC_FRAME_BYTES];
                if(dmr_codec_frameIsVoice(frame))
                    dmrAudio_pushFrame(frame);
            }
        }
    }

    /*
     * A newly confirmed call. Whether it is ours to receive is policy, and
     * policy is decided here, with the radio's settings at hand: the call
     * state knows how to tell a call, not whether we want it.
     */
    if(events & DMR_CALLSTATE_NEW_CALL)
    {
        bool     group = (cs->candidate.callType == DMR_CALL_GROUP);
        uint32_t dstId = cs->candidate.dstId;
        uint32_t srcId = cs->candidate.srcId;

        if(callAccepted(group, dstId) == false)
        {
            dmrDebug_log(DMR_DBG_INFO, "call: ignored, from %lu to %s %lu",
                         (unsigned long) srcId, group ? "TG" : "ID",
                         (unsigned long) dstId);

            /*
             * A call in progress is dropped along with the rejected one:
             * playback follows the call state, so leaving it up would let
             * the rejected call be heard through the tail of the one it
             * interrupted.
             */
            if(cs->call.valid)
                dmrAudio_stopRx();

            dmr_callState_reject(cs);
        }
        else
        {
            dmr_callState_accept(cs);

            dmrDebug_log(DMR_DBG_INFO, "call: from %lu to %s %lu",
                         (unsigned long) srcId, group ? "TG" : "ID",
                         (unsigned long) dstId);

            /* Start voice playback, if the vocoder is installed. */
            if(dmrAudio_startRx())
                dmrDebug_log(DMR_DBG_INFO, "audio: voice playback started");
        }
    }
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

    memset(txLc, 0, sizeof(txLc));
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
    for(int slot = 0; slot < DMR_SLOT_COUNT; slot++)
        dmr_callState_init(&rxCall[slot]);

    txState         = TX_IDLE;
    txStopRequested = false;
    rxRestoreNeeded = false;

    dmrCfg           = *cfg;
    dmrCfg.colorCode = cfg->colorCode & 0x0F;
    lastSysCount  = 0;
    lastTsCount   = 0;
    reportedTs   = 0;
    reportedSys  = 0;
    reportedTx   = 0;
    reportTick   = getTick();

    rxSampled         = 0;
    reportedUnsampled = 0;
    memset(rxSyncSeen, 0, sizeof(rxSyncSeen));

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

    for(int slot = 0; slot < DMR_SLOT_COUNT; slot++)
        dmr_callState_init(&rxCall[slot]);

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

    dmr_callState_resetLcTracking(&rxCall[0]);

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

        dmr_callState_restartTracking(&rxCall[0], getTick());

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

        handleBurst();
    }

    dmrIrq_setBusBusy(false);

    /*
     * The call state decides whether the call is over - behind a confirmed
     * terminator or by the carrier going away - and this is where its
     * decision is acted on: the audio and the log are hardware and bench
     * concerns, so they stay on this side of the boundary.
     */
    struct dmrCallState *cs = &rxCall[0];

    enum dmrCallEnd endReason = dmr_callState_update(cs, getTick());
    if(endReason != DMR_CALL_END_NONE)
    {
        dmrAudio_stopRx();
        dmrDebug_log(DMR_DBG_INFO, "call: ended (%s)",
                     (endReason == DMR_CALL_END_TERMINATOR) ? "terminator"
                                                            : "timeout");
    }

    reportIrqActivity();

    rx->srcId       = cs->call.srcId;
    rx->dstId       = cs->call.dstId;
    rx->colorCode   = cs->call.colourCode;
    rx->groupCall   = (cs->call.callType == DMR_CALL_GROUP);
    rx->valid       = cs->call.valid;
    rx->voiceActive = cs->call.voiceActive;
}

#endif /* CONFIG_DMR */
