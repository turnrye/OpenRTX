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
    TX_END_2            ///< Return to reception.
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

static volatile uint8_t txState;        // Current transmit slot state
static volatile uint8_t txSequence;     // Voice frame sequence, 0..5
static volatile bool    txStopRequested;// Unkey asked, waiting for a boundary
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
                 configuredCc);
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

    reportedTs  = counters.timeslot;
    reportedSys = counters.system;
    reportedTx  = counters.transmit;
    reportTick  = now;
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
    if(dmrDebug_channelEnabled(DMR_DBG_VP))
    {
        uint8_t record[1 + VP_LENGTH];

        record[0] = burstStatus;
        payloadRead(&record[1]);
        dmrDebug_record(DMR_DBG_VP, "VP", record, sizeof(record));
    }

    if(crcOk == false)
        return;

    if((status.rxStatus & RX_STATUS_PRIVACY) != 0)
    {
        dmrDebug_log(DMR_DBG_LC, "burst: rejected, encrypted");
        return;
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
        return;
    }

    if(memcmp(lc, prevLc, sizeof(lc)) != 0)
    {
        memcpy(prevLc, lc, sizeof(lc));
        lcMatches = 1;
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
        dmrDebug_log(DMR_DBG_INFO, "call: from %lu to %s %lu, color code %u",
                     (unsigned long) srcId, group ? "TG" : "ID",
                     (unsigned long) dstId, rxCc);
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
         * A voice frame: load the silence payload on the second bus, then set
         * the slot to transmit it with the current sequence number in the burst
         * type high nibble, and advance the sequence.
         */
        case TX_VOICE:
            payloadWrite(TX_SILENCE_AUDIO);
            C6000.writeCfgRegisterFromIsr(TX_REG_SLOT_ACTION, TX_SLOT_TRANSMIT);
            C6000.writeCfgRegisterFromIsr(TX_REG_BURST_TYPE,
                                          TX_BURST_VOICE | (txSequence << 4));
            txSequence = (txSequence + 1) % TX_SUPERFRAME_FRAMES;
            txState = TX_GAP;
            break;

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
         * One slot later the terminator has been sent, so put the slot timing
         * back to reception and stop driving the slot. Going idle here is what
         * the transmitting thread waits on to hand back to the receiver.
         */
        case TX_END_2:
            C6000.writeCfgRegisterFromIsr(0x40,
                static_cast< uint8_t >(C6000_DmrTiming::RX_IDLE));
            C6000.writeCfgRegisterFromIsr(TX_REG_SLOT_ACTION, TX_SLOT_IDLE);
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

void dmr_init(const uint8_t colorCode)
{
    memset(&rxState, 0, sizeof(rxState));
    memset(prevLc, 0, sizeof(prevLc));

    txState         = TX_IDLE;
    txStopRequested = false;

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
    payloadBusInit();

    /*
     * The receiver idles in passive timing so the chip can lock to the slot
     * clock of whoever transmits. Active timing made it free run instead:
     * the timeslot line ticked at a steady 33Hz while a transmission on
     * frequency produced no system events at all.
     */
    C6000.setDmrColorCode(configuredCc);
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
    dmrIrq_terminate();

    txState         = TX_IDLE;
    txStopRequested = false;

    C6000.setDmrSlotAction(C6000_DmrSlotAction::IDLE);
    payloadBusTerminate();
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
    if(rxState.valid && ((getTick() - lastBurstTick) > RX_HOLD_TIMEOUT))
    {
        rxState.valid = false;
        memset(prevLc, 0, sizeof(prevLc));
        lcMatches = 0;

        dmrDebug_log(DMR_DBG_INFO, "call: ended");
    }

    reportIrqActivity();

    *rx = rxState;
}

#endif /* CONFIG_DMR */
