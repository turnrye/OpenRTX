/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "peripherals/gpio.h"
#include "hwconfig.h"
#include "dmrIrq.h"
#include "dmr_C6000.h"
#include <kernel/scheduler/scheduler.h>
#include <miosix.h>

using namespace miosix;

#if defined(CONFIG_DMR)

/* Register holding the received colour code in its upper nibble. */
#define C6000_REG_CACH 0x52

/*
 * Register holding the status of the burst just decoded: bits 7..4 the data
 * type or voice frame sequence number, bit 2 the CRC result, bits 1..0 the
 * sync class (1 = voice). Like the colour code it is only valid at the frame
 * event, so it is sampled here rather than from the consuming thread.
 */
#define C6000_REG_RX_STATUS 0x51

/* Colour code value standing for "nothing received yet". */
#define CC_NONE 0xFF

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
static volatile bool stormSeen;      /* Lines shut down by the guard */
static volatile uint8_t rxColorCode = CC_NONE; /* CC of the last frame event */
static volatile uint8_t rxBurstStatus; /* 0x51 of the last frame event */
static volatile bool busBusy;          /* A thread is on the SPI bus  */
static volatile uint32_t
    pendingEvents;      /* Events not yet taken, DMR_IRQ_EVENT_* */
static Thread *waiting; /* Thread waiting for an event, if any */

/**
 * Shut the interrupt lines down when they fire far too often to be real. A
 * line stuck low would otherwise keep re-entering the handler and no thread
 * would ever run again, which looks exactly like the radio locking up.
 */
static inline void stormGuard()
{
    stormCount += 1;

    if (stormCount > IRQ_STORM_LIMIT) {
        EXTI->IMR &= ~(EXTI_IMR_MR0 | EXTI_IMR_MR1 | EXTI_IMR_MR2);
        stormSeen = true;
    }
}

/**
 * Record an event and wake the thread waiting for one, if any. Called with the
 * interrupts disabled.
 *
 * @param event: DMR_IRQ_EVENT_* bit to record.
 * @return true if the woken thread should preempt the current one.
 */
static inline bool IRQsignal(const uint32_t event)
{
    pendingEvents |= event;

    if (waiting == nullptr)
        return false;

    Thread *woken = waiting;
    waiting = nullptr;
    woken->IRQwakeup();

    return woken->IRQgetPriority()
         > Thread::IRQgetCurrentThread()->IRQgetPriority();
}

/*
 * The three interrupt lines have one EXTI line each, so the handlers do not
 * have to tell apart which pin fired. The timeslot and system handlers can
 * wake a thread and switch to it on the way out, so they run behind naked
 * wrappers saving and restoring the full context.
 */

void __attribute__((used)) dmrTimeslotIrqImpl()
{
    EXTI->PR = EXTI_PR_PR0;
    tsCount += 1;

    /*
     * The DMR thread sets up each slot, transmitting as well as receiving,
     * straight after the boundary.
     */
    if (IRQsignal(DMR_IRQ_EVENT_TIMESLOT))
        Scheduler::IRQfindNextThread();

    stormGuard();
}

void __attribute__((naked)) EXTI0_IRQHandler()
{
    saveContext();
    asm volatile("bl _Z18dmrTimeslotIrqImplv");
    restoreContext();
}

void __attribute__((used)) dmrSystemIrqImpl()
{
    EXTI->PR = EXTI_PR_PR1;
    sysCount += 1;

    /*
     * The flags and the burst they describe are read by the DMR thread, which
     * gets to them well within the slot the chip holds them for.
     */
    if (IRQsignal(DMR_IRQ_EVENT_SYSTEM))
        Scheduler::IRQfindNextThread();

    stormGuard();
}

void __attribute__((naked)) EXTI1_IRQHandler()
{
    saveContext();
    asm volatile("bl _Z16dmrSystemIrqImplv");
    restoreContext();
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
    pendingEvents = 0;

    /*
     * The lines are active on the falling edge, so they idle high: pulling
     * them up keeps a line the chip is not driving from tripping on noise.
     */
    gpio_setMode(DMR_TS_INT, INPUT_PULL_UP);
    gpio_setMode(DMR_SYS_INT, INPUT_PULL_UP);
    gpio_setMode(DMR_TX_INT, INPUT_PULL_UP);

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
    SYSCFG->EXTICR[0] |= SYSCFG_EXTICR1_EXTI0_PC | SYSCFG_EXTICR1_EXTI1_PC
                       | SYSCFG_EXTICR1_EXTI2_PC;

    EXTI->FTSR |= EXTI_FTSR_TR0 | EXTI_FTSR_TR1 | EXTI_FTSR_TR2;
    EXTI->RTSR &= ~(EXTI_RTSR_TR0 | EXTI_RTSR_TR1 | EXTI_RTSR_TR2);
    EXTI->PR = EXTI_PR_PR0 | EXTI_PR_PR1 | EXTI_PR_PR2;
    EXTI->IMR |= EXTI_IMR_MR0 | EXTI_IMR_MR1 | EXTI_IMR_MR2;

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
    counters->system = sysCount;
    counters->transmit = txCount;
}

void dmrIrq_clearCounters()
{
    tsCount = 0;
    sysCount = 0;
    txCount = 0;
    rxColorCode = CC_NONE;
    rxBurstStatus = 0;
}

void dmrIrq_feedWatchdog()
{
    stormCount = 0;
}

bool dmrIrq_stormDetected()
{
    return stormSeen;
}

uint8_t dmrIrq_getRxColorCode()
{
    return rxColorCode;
}

uint8_t dmrIrq_getRxBurstStatus()
{
    return rxBurstStatus;
}

void dmrIrq_setBusBusy(bool busy)
{
    busBusy = busy;
}

uint32_t dmrIrq_waitEvents()
{
    FastInterruptDisableLock dLock;

    while (pendingEvents == 0) {
        waiting = Thread::IRQgetCurrentThread();
        Thread::IRQwait();
        {
            FastInterruptEnableLock eLock(dLock);
            Thread::yield();
        }
    }

    uint32_t events = pendingEvents;
    pendingEvents = 0;

    return events;
}

uint32_t dmrIrq_takeEvents()
{
    FastInterruptDisableLock dLock;

    uint32_t events = pendingEvents;
    pendingEvents = 0;

    return events;
}

void dmrIrq_wakeWaiter()
{
    /*
     * From a thread the switch cannot be made with the interrupts disabled:
     * wake the waiter, then yield to it once they are enabled again.
     */
    bool preempt;
    {
        FastInterruptDisableLock dLock;
        preempt = IRQsignal(DMR_IRQ_EVENT_WAKE);
    }

    if (preempt)
        Thread::yield();
}

#endif /* CONFIG_DMR */
