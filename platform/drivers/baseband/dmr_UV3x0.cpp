/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/delays.h"
#include "core/dmr_debug.h"
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
 * The chip hands back the link control of the previous transmission until it
 * has decoded the current one, so the same content has to show up in more than
 * one burst before it can be trusted.
 */
#define LC_CONFIRM_COUNT        2

/* Interval between two interrupt activity reports, in milliseconds */
#define IRQ_REPORT_INTERVAL     1000

static struct dmrRxState rxState;
static uint8_t   configuredCc;
static uint8_t   prevLc[C6000_LC_LENGTH];
static uint8_t   lcMatches;
static uint32_t  lastSysCount;
static uint32_t  reportedTs;
static uint32_t  reportedSys;
static uint32_t  reportedTx;
static long long reportTick;

/**
 * Read back what was written during the initialisation. A mismatch here means
 * the SPI interface towards the baseband is not working at all, which is worth
 * telling apart from a chip which is configured but receiving nothing.
 */
static void checkInitRegisters()
{
    uint8_t code   = C6000.getDmrColorCode();
    uint8_t timing = C6000.getDmrTiming();
    uint8_t wanted = static_cast< uint8_t >(C6000_DmrTiming::ACTIVE);
    bool    ok     = (code == configuredCc) && (timing == wanted);

    dmrDebug_log(DMR_DBG_INFO,
                 "init: color code %u want %u, timing %02X want %02X: %s",
                 code, configuredCc, timing, wanted, ok ? "PASS" : "FAIL");
}

/**
 * Report how often each interrupt line fired since the last report. With the
 * chip in active timing the timeslot line runs at about 33Hz, a count of zero
 * means the chip is not generating its slot timing or the interrupt path is
 * not working.
 */
static void reportIrqActivity()
{
    long long now = getTick();

    if((now - reportTick) < IRQ_REPORT_INTERVAL)
        return;

    struct dmrIrqCounters counters;
    dmrIrq_getCounters(&counters);

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

    dmrDebug_log(DMR_DBG_LC,
                 "burst: flags %02X status %02X cach %02X sync %02X cc %u crc %s",
                 status.intFlags, status.rxStatus, status.cach,
                 status.syncStatus, code, crcOk ? "ok" : "bad");

    if(crcOk == false)
        return;

    if((status.rxStatus & RX_STATUS_PRIVACY) != 0)
    {
        dmrDebug_log(DMR_DBG_LC, "burst: rejected, encrypted");
        return;
    }

    if(code != configuredCc)
    {
        dmrDebug_log(DMR_DBG_LC, "burst: rejected, color code %u want %u",
                     code, configuredCc);
        return;
    }

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

    lcMatches += 1;
    if(lcMatches < LC_CONFIRM_COUNT)
        return;

    rxState.dstId     = (lc[3] << 16) | (lc[4] << 8) | lc[5];
    rxState.srcId     = (lc[6] << 16) | (lc[7] << 8) | lc[8];
    rxState.colorCode = code;
    rxState.groupCall = (lc[0] == LC_FLCO_GROUP);
    rxState.valid     = true;

    dmrDebug_log(DMR_DBG_LC, "call: from %lu to %s %lu, color code %u",
                 (unsigned long) rxState.srcId,
                 rxState.groupCall ? "TG" : "ID",
                 (unsigned long) rxState.dstId, code);
}

void dmr_init(const uint8_t colorCode)
{
    memset(&rxState, 0, sizeof(rxState));
    memset(prevLc, 0, sizeof(prevLc));

    configuredCc = colorCode & 0x0F;
    lcMatches    = 0;
    lastSysCount = 0;
    reportedTs   = 0;
    reportedSys  = 0;
    reportedTx   = 0;
    reportTick   = getTick();

    dmrIrq_init();

    C6000.setDmrColorCode(configuredCc);
    C6000.setDmrTiming(C6000_DmrTiming::ACTIVE);
    C6000.setDmrSlotAction(C6000_DmrSlotAction::RECEIVE);

    checkInitRegisters();
}

void dmr_terminate()
{
    dmrIrq_terminate();

    C6000.setDmrSlotAction(C6000_DmrSlotAction::IDLE);
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
     * The chip signals a frame event on the system interrupt line, but reading
     * it out has to happen here: the interrupt handlers cannot use the SPI
     * interface towards the baseband.
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

    reportIrqActivity();

    *rx = rxState;
}

#endif /* CONFIG_DMR */
