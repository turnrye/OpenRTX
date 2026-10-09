/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "peripherals/gpio.h"
#include "hwconfig.h"
#include "dmrIrq.h"
#include "paRamp_CS7000.h"
#include <kernel/scheduler/scheduler.h>
#include <miosix.h>

using namespace miosix;

#if defined(CONFIG_DMR)

/*
 * Events tolerated between two watchdog feeds before the lines are considered
 * stuck. A slot boundary comes every 30ms, so even the busiest run is around
 * one hundred events per feed: anything approaching this limit is a line
 * firing on noise rather than on the chip.
 */
#define IRQ_STORM_LIMIT 3000

/*
 * The three lines on the CS7000 Plus: timeslot on PC0, transmit on PC2 and
 * system on PC3, each on the EXTI line of the same number.
 */
#define LINES_MASK (EXTI_IMR1_IM0 | EXTI_IMR1_IM2 | EXTI_IMR1_IM3)

static volatile uint32_t tsCount;    /* Timeslot interrupt counter  */
static volatile uint32_t sysCount;   /* System interrupt counter    */
static volatile uint32_t txCount;    /* Transmit interrupt counter  */
static volatile uint32_t stormCount; /* Events since the last feed  */
static volatile bool stormSeen;      /* Lines shut down by the guard */
static volatile bool busBusy;        /* The DMR thread is on the SPI bus */
static volatile uint32_t
    pendingEvents;                   /* Events not yet taken, DMR_IRQ_EVENT_* */
static Thread *waiting;              /* Thread waiting for an event, if any */

/**
 * Shut the interrupt lines down when they fire far too often to be real. A
 * line stuck low would otherwise keep re-entering the handler and no thread
 * would ever run again, which looks exactly like the radio locking up.
 */
static inline void stormGuard()
{
    stormCount += 1;

    if (stormCount > IRQ_STORM_LIMIT) {
        EXTI->IMR1 &= ~LINES_MASK;
        stormSeen = true;
    }
}

/**
 * Record an event and wake the thread waiting for one, if any. Called with the
 * interrupts disabled.
 *
 * @param event: DMR_IRQ_EVENT_* bits to record.
 * @return true if the woken thread should preempt the running one.
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
 * The timeslot and system handlers can wake the reception thread and switch
 * to it straight away, which in Miosix takes a handler saving and restoring
 * the interrupted context itself: the handlers proper run from naked wrappers.
 */
void __attribute__((used)) dmrTimeslotIrqImpl()
{
    EXTI->PR1 = EXTI_PR1_PR0;
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

void __attribute__((used)) EXTI2_IRQHandler()
{
    EXTI->PR1 = EXTI_PR1_PR2;
    txCount += 1;

    /* The chip is about to send a burst: ramp the PA up for it. */
    paRamp_burstIsr();

    stormGuard();
}

void __attribute__((used)) dmrSystemIrqImpl()
{
    EXTI->PR1 = EXTI_PR1_PR3;
    sysCount += 1;

    /*
     * The flags and the burst they describe are read by the reception thread,
     * which gets to them well within the slot the chip holds them for.
     */
    if (IRQsignal(DMR_IRQ_EVENT_SYSTEM))
        Scheduler::IRQfindNextThread();

    stormGuard();
}

void __attribute__((naked)) EXTI3_IRQHandler()
{
    saveContext();
    asm volatile("bl _Z16dmrSystemIrqImplv");
    restoreContext();
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

    RCC->APB4ENR |= RCC_APB4ENR_SYSCFGEN;
    __DSB();

    /* Route EXTI lines 0, 2 and 3 to port C. */
    SYSCFG->EXTICR[0] = (SYSCFG->EXTICR[0]
                         & ~(SYSCFG_EXTICR1_EXTI0 | SYSCFG_EXTICR1_EXTI2
                             | SYSCFG_EXTICR1_EXTI3))
                      | SYSCFG_EXTICR1_EXTI0_PC | SYSCFG_EXTICR1_EXTI2_PC
                      | SYSCFG_EXTICR1_EXTI3_PC;

    EXTI->FTSR1 |= EXTI_FTSR1_TR0 | EXTI_FTSR1_TR2 | EXTI_FTSR1_TR3;
    EXTI->RTSR1 &= ~(EXTI_RTSR1_TR0 | EXTI_RTSR1_TR2 | EXTI_RTSR1_TR3);
    EXTI->PR1 = EXTI_PR1_PR0 | EXTI_PR1_PR2 | EXTI_PR1_PR3;
    EXTI->IMR1 |= LINES_MASK;

    /*
     * Lowest priority, as the handlers only touch their own counters and the
     * slot boundaries are 30ms apart.
     */
    NVIC_ClearPendingIRQ(EXTI0_IRQn);
    NVIC_ClearPendingIRQ(EXTI2_IRQn);
    NVIC_ClearPendingIRQ(EXTI3_IRQn);
    NVIC_SetPriority(EXTI0_IRQn, 15);
    NVIC_SetPriority(EXTI2_IRQn, 15);
    NVIC_SetPriority(EXTI3_IRQn, 15);
    NVIC_EnableIRQ(EXTI0_IRQn);
    NVIC_EnableIRQ(EXTI2_IRQn);
    NVIC_EnableIRQ(EXTI3_IRQn);
}

void dmrIrq_terminate()
{
    EXTI->IMR1 &= ~LINES_MASK;

    NVIC_DisableIRQ(EXTI0_IRQn);
    NVIC_DisableIRQ(EXTI2_IRQn);
    NVIC_DisableIRQ(EXTI3_IRQn);
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
}

void dmrIrq_feedWatchdog()
{
    stormCount = 0;
}

bool dmrIrq_stormDetected()
{
    return stormSeen;
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
