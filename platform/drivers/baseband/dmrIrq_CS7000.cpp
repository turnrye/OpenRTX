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

/* Colour code value standing for "nothing received yet". */
#define CC_NONE         0xFF

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
#define LINES_MASK      (EXTI_IMR1_IM0 | EXTI_IMR1_IM2 | EXTI_IMR1_IM3)

static volatile uint32_t tsCount;    /* Timeslot interrupt counter  */
static volatile uint32_t sysCount;   /* System interrupt counter    */
static volatile uint32_t txCount;    /* Transmit interrupt counter  */
static volatile uint32_t stormCount; /* Events since the last feed  */
static volatile bool     stormSeen;  /* Lines shut down by the guard */
static volatile uint8_t  rxColorCode = CC_NONE; /* CC of the last frame event */
static volatile uint8_t  rxBurstStatus;         /* 0x51 of the last frame event */
static volatile bool     busBusy;    /* The DMR thread is on the SPI bus */

/**
 * Tell whether the handlers may reach the chip right now. The control bus is
 * a hardware SPI shared with other threads, the DAC FIFO feeding the speaker
 * among them, and a handler cannot take its mutex: it stays off the bus while
 * any thread is on it.
 */
static inline bool busFree()
{
    return (busBusy == false) && (ScopedChipSelect::busActive() == false);
}

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
        EXTI->IMR1 &= ~LINES_MASK;
        stormSeen   = true;
    }
}

void __attribute__((used)) EXTI0_IRQHandler()
{
    EXTI->PR1 = EXTI_PR1_PR0;
    tsCount += 1;

    /*
     * The timeslot interrupt is the slot clock the transmit state machine runs
     * on: while a transmission is active it sets up the next burst here, within
     * the slot deadline the thread cannot meet.
     */
    if(dmr_txActive() && busFree())
        dmr_txTimeslotIsr();

    stormGuard();
}

void __attribute__((used)) EXTI2_IRQHandler()
{
    EXTI->PR1 = EXTI_PR1_PR2;
    txCount += 1;
    stormGuard();
}

void __attribute__((used)) EXTI3_IRQHandler()
{
    EXTI->PR1 = EXTI_PR1_PR3;
    sysCount += 1;

    /*
     * The burst status and colour code registers are only valid at the instant
     * of the frame event, so they are read here rather than from the consuming
     * thread, unless a thread holds the bus; the next event picks them up. They
     * are also skipped while transmitting, when a receive read has nothing to
     * say.
     */
    if(busFree() && (dmr_txActive() == false))
    {
        rxBurstStatus = C6000.readCfgRegisterFromIsr(C6000_REG_RX_STATUS);
        rxColorCode   = (C6000.readCfgRegisterFromIsr(C6000_REG_CACH) >> 4) & 0x0F;
    }

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

    RCC->APB4ENR |= RCC_APB4ENR_SYSCFGEN;
    __DSB();

    /* Route EXTI lines 0, 2 and 3 to port C. */
    SYSCFG->EXTICR[0] = (SYSCFG->EXTICR[0]
                        & ~(SYSCFG_EXTICR1_EXTI0 | SYSCFG_EXTICR1_EXTI2
                            | SYSCFG_EXTICR1_EXTI3))
                      | SYSCFG_EXTICR1_EXTI0_PC
                      | SYSCFG_EXTICR1_EXTI2_PC
                      | SYSCFG_EXTICR1_EXTI3_PC;

    EXTI->FTSR1 |= EXTI_FTSR1_TR0 | EXTI_FTSR1_TR2 | EXTI_FTSR1_TR3;
    EXTI->RTSR1 &= ~(EXTI_RTSR1_TR0 | EXTI_RTSR1_TR2 | EXTI_RTSR1_TR3);
    EXTI->PR1    = EXTI_PR1_PR0 | EXTI_PR1_PR2 | EXTI_PR1_PR3;
    EXTI->IMR1  |= LINES_MASK;

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
    counters->system   = sysCount;
    counters->transmit = txCount;
}

void dmrIrq_clearCounters()
{
    tsCount       = 0;
    sysCount      = 0;
    txCount       = 0;
    rxColorCode   = CC_NONE;
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

#endif /* CONFIG_DMR */
