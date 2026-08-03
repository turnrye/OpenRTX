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
#define VP_REGISTER             0x00

/*
 * The chip hands back the link control of the previous transmission until it
 * has decoded the current one, so the same content has to show up in more than
 * one burst before it can be trusted.
 */
#define LC_CONFIRM_COUNT        2

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
     * Both colour code sources are logged side by side: "cc" is register 0x52
     * read here, "isrcc" is the same register sampled in the system interrupt.
     * Watching the two against a known transmitted colour code says whether
     * either one can be trusted for filtering.
     */
    dmrDebug_log(DMR_DBG_LC,
                 "burst: flags %02X status %02X cach %02X sync %02X cc %u "
                 "isrcc %u crc %s", status.intFlags, status.rxStatus,
                 status.cach, status.syncStatus, code,
                 dmrIrq_getRxColorCode(), crcOk ? "ok" : "bad");

    /*
     * Stream the raw voice payload for the host-side capture harness. This is
     * done before any of the filters below and independently of the CRC check:
     * a voice burst carries no fresh link control and fails that check, so
     * gating the capture on it would drop exactly the bursts worth recording.
     * The receive status byte is prepended so the host can tell voice frames
     * apart from the sync headers and terminators that share the stream.
     */
    if(dmrDebug_channelEnabled(DMR_DBG_VP))
    {
        uint8_t record[1 + VP_LENGTH];

        record[0] = status.rxStatus;
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

void dmr_init(const uint8_t colorCode)
{
    memset(&rxState, 0, sizeof(rxState));
    memset(prevLc, 0, sizeof(prevLc));

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
