/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "peripherals/gpio.h"
#include "hwconfig.h"
#include "dmrIrq.h"

#if defined(CONFIG_DMR)

static volatile uint32_t tsCount;    /* Timeslot interrupt counter  */
static volatile uint32_t sysCount;   /* System interrupt counter    */
static volatile uint32_t txCount;    /* Transmit interrupt counter  */

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
}

void __attribute__((used)) EXTI1_IRQHandler()
{
    EXTI->PR = EXTI_PR_PR1;
    sysCount += 1;
}

void __attribute__((used)) EXTI2_IRQHandler()
{
    EXTI->PR = EXTI_PR_PR2;
    txCount += 1;
}

void dmrIrq_init()
{
    dmrIrq_clearCounters();

    gpio_setMode(DMR_TS_INT,  INPUT);
    gpio_setMode(DMR_SYS_INT, INPUT);
    gpio_setMode(DMR_TX_INT,  INPUT);

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
    tsCount  = 0;
    sysCount = 0;
    txCount  = 0;
}

#endif /* CONFIG_DMR */
