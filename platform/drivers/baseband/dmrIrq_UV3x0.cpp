/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "peripherals/gpio.h"
#include "hwconfig.h"
#include "dmrIrq.h"
#include "dmr.h"

#if defined(CONFIG_DMR)

/* Register holding the received colour code in its upper nibble. */
#define C6000_REG_CACH  0x52

/*
 * Register holding the status of the burst just decoded: bits 7..4 the data
 * type or voice frame sequence number, bit 2 the CRC result, bits 1..0 the
 * sync class (1 = voice). Like the colour code it is only valid at the frame
 * event, so it is sampled here rather than from the consuming thread.
 */
#define C6000_REG_RX_STATUS 0x51

/* Register naming the sync pattern the chip matched, in its low two bits. */
#define C6000_REG_RX_SYNC   0x5F

/*
 * Two registers being hunted for a direct mode colour code, which neither of
 * the obvious ones carries. 0x57 is where the stock firmware is said to read
 * the timecode, rather than from 0x52, so its neighbourhood is worth watching.
 * 0x90 is the sub-status of the received-information interrupt, which was
 * masked off until the interrupt mask was opened up.
 */
#define C6000_REG_TIMECODE  0x57
#define C6000_REG_RX_INFO   0x90

/* Register holding the interrupt flags, which say what raised the event. */
#define C6000_REG_INT_FLAGS 0x82

/*
 * Events tolerated between two watchdog feeds before the lines are considered
 * stuck. A slot boundary comes every 30ms, so even the busiest run is around
 * one hundred events per feed: anything approaching this limit is a line
 * firing on noise rather than on the chip.
 */
#define IRQ_STORM_LIMIT 3000

static volatile uint32_t tsCount;    /* Timeslot interrupt counter  */
static volatile uint32_t sysCount;   /* System interrupt counter    */
static volatile uint32_t txCount;    /* Transmit interrupt counter  */
static volatile uint32_t stormCount; /* Events since the last feed  */
static volatile bool     stormSeen;  /* Lines shut down by the guard */
static volatile bool     busBusy;    /* A thread is on the SPI bus  */

/* Registers of the last frame event, and the events which went unsampled. */
static volatile struct dmrIrqRxSnapshot rxSnapshot;
static volatile uint32_t rxUnsampled;

/**
 * Shut the interrupt lines down when they fire far too often to be real. A
 * line stuck low would otherwise keep re-entering the handler and no thread
 * would ever run again, which looks exactly like the radio locking up.
 */
static inline void stormGuard()
{
    stormCount += 1;

    if(stormCount > IRQ_STORM_LIMIT)
    {
        EXTI->IMR &= ~(EXTI_IMR_MR0 | EXTI_IMR_MR1 | EXTI_IMR_MR2);
        stormSeen  = true;
    }
}

/*
 * The three interrupt lines have one EXTI line each, so the handlers do not
 * have to tell apart which pin fired. Incrementing the counter is all they do:
 * anything reading the chip has to go through the bitbanged SPI, which is not
 * usable from an interrupt context.
 */

void __attribute__((used)) EXTI0_IRQHandler()
{
    EXTI->PR = EXTI_PR_PR0;
    tsCount += 1;

    /*
     * The timeslot interrupt is the slot clock the transmit state machine runs
     * on: while a transmission is active it sets up the next burst here, within
     * the slot deadline the thread cannot meet. It is fenced off while a thread
     * holds the control bus, exactly as the system interrupt's read below is.
     */
    if(dmr_txActive() && (busBusy == false))
        dmr_txTimeslotIsr();

    stormGuard();
}

void __attribute__((used)) EXTI1_IRQHandler()
{
    EXTI->PR = EXTI_PR_PR1;
    sysCount += 1;

    /*
     * The registers describing the burst are only valid at the instant of the
     * frame event, so they are read here rather than from the consuming thread.
     * The reads are skipped while a thread holds the bus, to avoid two contexts
     * driving the bitbanged SPI at once; the next event picks them up. They are
     * also skipped while transmitting, when the timeslot interrupt is the one
     * driving the bus and a receive read would have nothing to say. A skipped
     * event leaves the snapshot describing an earlier burst, so it is counted:
     * a reading of the snapshot is only trustworthy while that count stays low.
     *
     * The order of the reads is the order in which the values go stale. The
     * interrupt flags are latched until they are cleared, so they come last.
     */
    if((busBusy == false) && (dmr_txActive() == false))
    {
        rxSnapshot.status   = C6000.readCfgRegisterFromIsr(C6000_REG_RX_STATUS);
        rxSnapshot.cach     = C6000.readCfgRegisterFromIsr(C6000_REG_CACH);
        rxSnapshot.timecode = C6000.readCfgRegisterFromIsr(C6000_REG_TIMECODE);
        rxSnapshot.rxInfo   = C6000.readCfgRegisterFromIsr(C6000_REG_RX_INFO);
        rxSnapshot.sync     = C6000.readCfgRegisterFromIsr(C6000_REG_RX_SYNC);
        rxSnapshot.intFlags = C6000.readCfgRegisterFromIsr(C6000_REG_INT_FLAGS);
        rxSnapshot.valid    = true;
    }
    else
    {
        rxUnsampled += 1;
    }

    stormGuard();
}

void __attribute__((used)) EXTI2_IRQHandler()
{
    EXTI->PR = EXTI_PR_PR2;
    txCount += 1;
    stormGuard();
}

void dmrIrq_init()
{
    dmrIrq_clearCounters();

    /*
     * The lines are active on the falling edge, so they idle high: pulling
     * them up keeps a line the chip is not driving from tripping on noise.
     */
    gpio_setMode(DMR_TS_INT,  INPUT_PULL_UP);
    gpio_setMode(DMR_SYS_INT, INPUT_PULL_UP);
    gpio_setMode(DMR_TX_INT,  INPUT_PULL_UP);

    /*
     * The SYSCFG clock is already enabled by the USB stack during boot, but
     * turning it on here too keeps the EXTI line routing below from depending
     * on that.
     */
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    __DSB();

    /*
     * The interrupt lines are on PC0, PC1 and PC2 and are active on the
     * falling edge.
     */
    SYSCFG->EXTICR[0] |= SYSCFG_EXTICR1_EXTI0_PC
                      |  SYSCFG_EXTICR1_EXTI1_PC
                      |  SYSCFG_EXTICR1_EXTI2_PC;

    EXTI->FTSR |= EXTI_FTSR_TR0 | EXTI_FTSR_TR1 | EXTI_FTSR_TR2;
    EXTI->RTSR &= ~(EXTI_RTSR_TR0 | EXTI_RTSR_TR1 | EXTI_RTSR_TR2);
    EXTI->PR    = EXTI_PR_PR0 | EXTI_PR_PR1 | EXTI_PR_PR2;
    EXTI->IMR  |= EXTI_IMR_MR0 | EXTI_IMR_MR1 | EXTI_IMR_MR2;

    /*
     * Lowest priority, as the handlers only touch their own counters and the
     * slot boundaries are 30ms apart.
     */
    NVIC_ClearPendingIRQ(EXTI0_IRQn);
    NVIC_ClearPendingIRQ(EXTI1_IRQn);
    NVIC_ClearPendingIRQ(EXTI2_IRQn);
    NVIC_SetPriority(EXTI0_IRQn, 15);
    NVIC_SetPriority(EXTI1_IRQn, 15);
    NVIC_SetPriority(EXTI2_IRQn, 15);
    NVIC_EnableIRQ(EXTI0_IRQn);
    NVIC_EnableIRQ(EXTI1_IRQn);
    NVIC_EnableIRQ(EXTI2_IRQn);
}

void dmrIrq_terminate()
{
    EXTI->IMR &= ~(EXTI_IMR_MR0 | EXTI_IMR_MR1 | EXTI_IMR_MR2);

    NVIC_DisableIRQ(EXTI0_IRQn);
    NVIC_DisableIRQ(EXTI1_IRQn);
    NVIC_DisableIRQ(EXTI2_IRQn);
}

void dmrIrq_getCounters(struct dmrIrqCounters *counters)
{
    counters->timeslot = tsCount;
    counters->system   = sysCount;
    counters->transmit = txCount;
}

void dmrIrq_clearCounters()
{
    tsCount     = 0;
    sysCount    = 0;
    txCount     = 0;
    rxUnsampled = 0;

    rxSnapshot.intFlags = 0;
    rxSnapshot.status   = 0;
    rxSnapshot.cach     = 0;
    rxSnapshot.timecode = 0;
    rxSnapshot.rxInfo   = 0;
    rxSnapshot.sync     = 0;
    rxSnapshot.valid    = false;
}

void dmrIrq_feedWatchdog()
{
    stormCount = 0;
}

bool dmrIrq_stormDetected()
{
    return stormSeen;
}

void dmrIrq_getRxSnapshot(struct dmrIrqRxSnapshot *snapshot)
{
    snapshot->intFlags = rxSnapshot.intFlags;
    snapshot->status   = rxSnapshot.status;
    snapshot->cach     = rxSnapshot.cach;
    snapshot->timecode = rxSnapshot.timecode;
    snapshot->rxInfo   = rxSnapshot.rxInfo;
    snapshot->sync     = rxSnapshot.sync;
    snapshot->valid    = rxSnapshot.valid;
}

uint32_t dmrIrq_getRxUnsampled()
{
    return rxUnsampled;
}

void dmrIrq_setBusBusy(bool busy)
{
    busBusy = busy;
}

#endif /* CONFIG_DMR */
