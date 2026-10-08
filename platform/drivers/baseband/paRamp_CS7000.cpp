/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/delays.h"
#include "peripherals/gpio.h"
#include "drivers/rcc.h"
#include "hwconfig.h"
#include "paRamp_CS7000.h"
#include <string.h>

/* Ramp step rate: one step every 50us. */
#define STEP_RATE       20000

static struct paRampShape next;         // Shape for the next burst
static struct paRampShape active;       // Shape of the ramp in progress
static volatile bool      armed;        // Ramping the bursts
static volatile bool      running;      // A ramp is in progress
static volatile uint16_t  step;         // Steps since the transmit interrupt
static bool               apcOn;        // APC switch set by the ramp
static bool               paOn;         // PA enabled by the ramp

void paRamp_init()
{
    armed   = false;
    running = false;

    /*
     * TIM4 on APB1, whose timers run at the bus clock as its prescaler is one:
     * count at 1MHz and overflow every 50us.
     */
    RCC->APB1LENR |= RCC_APB1LENR_TIM4EN;
    __DSB();

    TIM4->CR1  = 0;
    TIM4->PSC  = (getBusClock(PERIPH_BUS_APB1) / 1000000) - 1;
    TIM4->ARR  = (1000000 / STEP_RATE) - 1;
    TIM4->EGR  = TIM_EGR_UG;
    TIM4->SR   = 0;
    TIM4->DIER = TIM_DIER_UIE;

    /* Above the DMR interrupt lines, which start the ramps. */
    NVIC_ClearPendingIRQ(TIM4_IRQn);
    NVIC_SetPriority(TIM4_IRQn, 14);
    NVIC_EnableIRQ(TIM4_IRQn);
}

void paRamp_arm(const struct paRampShape *shape)
{
    __disable_irq();
    memcpy(&next, shape, sizeof(next));
    armed = true;
    __enable_irq();
}

/**
 * Switch everything the ramp drives off, APC output last.
 */
static void rampOff()
{
    gpioDev_clear(TX_PWR_EN);
    gpioDev_clear(RF_APC_SW);
    DAC1->DHR8R1 = 0;

    paOn  = false;
    apcOn = false;
}

void paRamp_disarm()
{
    armed = false;

    /* A ramp lasts about 32ms: let the one in progress finish. */
    for(int i = 0; (i < 40) && running; i++)
        sleepFor(0u, 1u);

    TIM4->CR1 &= ~TIM_CR1_CEN;
    running = false;
    rampOff();
}

void paRamp_burstIsr()
{
    if((armed == false) || running)
        return;

    memcpy(&active, &next, sizeof(active));

    step    = 0;
    running = true;
    paOn    = false;
    apcOn   = false;

    /* The APC output also tunes the receiver: start from zero. */
    DAC1->DHR8R1 = 0;

    TIM4->CNT  = 0;
    TIM4->SR   = 0;
    TIM4->CR1 |= TIM_CR1_CEN;
}

void __attribute__((used)) TIM4_IRQHandler()
{
    TIM4->SR = 0;

    if(running == false)
    {
        TIM4->CR1 &= ~TIM_CR1_CEN;
        return;
    }

    uint16_t s      = ++step;
    uint16_t upLast = active.start + active.upLen - 1;
    uint16_t end    = active.holdEnd + active.downLen;

    /* Waiting for the ramp up to start. */
    if(s < active.start)
        return;

    /* Ramp up: APC switch on first, then the PA part way up. */
    if(s <= upLast)
    {
        if(apcOn == false)
        {
            gpioDev_set(RF_APC_SW);
            apcOn = true;
        }

        DAC1->DHR8R1 = active.up[s - active.start];

        if((paOn == false)
           && ((s >= (active.start + active.paOn)) || (s == upLast)))
        {
            gpioDev_set(TX_PWR_EN);
            paOn = true;
        }

        return;
    }

    /* Hold for the burst. */
    if(s <= active.holdEnd)
        return;

    /* Ramp down, turning the PA off part way down. */
    if(s < end)
    {
        DAC1->DHR8R1 = active.down[end - s];

        if(paOn && (s >= (active.holdEnd + active.paOff)))
        {
            gpioDev_clear(TX_PWR_EN);
            gpioDev_clear(RF_APC_SW);
            paOn  = false;
            apcOn = false;
        }

        return;
    }

    /* Done: everything off until the next burst. */
    TIM4->CR1 &= ~TIM_CR1_CEN;
    rampOff();
    running = false;
}
