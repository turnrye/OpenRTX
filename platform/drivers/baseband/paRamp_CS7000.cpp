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

/* Highest APC value on the ramp up. */
#define APC_MAX         0xFA

/*
 * The last step of a ramp, from the transmit interrupt: a DMR burst lasts
 * 27.5ms and the next one of the same transmission comes 60ms after it, so a
 * ramp ending later than this would be calibration gone wrong.
 */
#define LAST_STEP       700

/*
 * Step of the ramp up turning the PA on, and of the ramp down turning it off,
 * as functions of the APC value: the stock firmware switches the PA later on
 * the way up and earlier on the way down the lower the output power. Each
 * entry applies from its APC value up to the next entry's.
 */
struct paThreshold
{
    uint8_t level;
    uint8_t step;
};

static const struct paThreshold PA_ON[] =
{
    {0x00, 36}, {0x3C, 34}, {0x41, 28}, {0x46, 24}, {0x4B, 26}, {0x50, 22},
    {0x5A, 17}, {0x5F, 18}, {0x64, 17}, {0x69, 16}, {0x73, 14}, {0x78, 13},
    {0x7D, 12}, {0x91, 11}, {0x96, 10}, {0x9B,  9}, {0xA5,  8}, {0xB4,  7},
    {0xC3,  6}, {0xE1,  5}, {0xF0,  4}, {0xF5,  3}
};

static const struct paThreshold PA_OFF[] =
{
    {0x00, 14}, {0x4B, 15}, {0x5F, 17}, {0xAF, 18}, {0xC3, 20}
};

static struct paRampShape next;         // Shape for the next burst
static struct paRampShape active;       // Shape of the ramp in progress
static volatile bool      armed;        // Ramping the bursts
static volatile bool      running;      // A ramp is in progress
static volatile uint16_t  step;         // Steps since the transmit interrupt
static bool               apcOn;        // APC switch set by the ramp
static bool               paOn;         // PA enabled by the ramp

static uint8_t lookup(const struct paThreshold *table, const size_t len,
                      const uint8_t level)
{
    uint8_t step = table[0].step;

    for(size_t i = 0; i < len; i++)
    {
        if(level >= table[i].level)
            step = table[i].step;
    }

    return step;
}

bool paRamp_build(struct paRampShape *shape, const uint8_t level,
                  const uint8_t start, const uint8_t upEnd,
                  const uint16_t holdEnd, const uint16_t downEnd)
{
    /* Calibration steps are 100us long, twice the ramp step. */
    int upLen   = 2 * (upEnd - start);
    int downLen = 2 * (downEnd - holdEnd);

    shape->start   = 2 * start;
    shape->holdEnd = 2 * holdEnd;

    if((level == 0) || (upLen < 5) || (upLen > PA_RAMP_MAX_STEPS)
       || (downLen < 1) || (downLen > PA_RAMP_MAX_STEPS)
       || ((shape->start + upLen) >= shape->holdEnd)
       || ((shape->holdEnd + downLen) > LAST_STEP))
        return false;

    shape->upLen   = upLen;
    shape->downLen = downLen;

    /*
     * Ramp up: from zero, a jump to about half the APC value, then linear up
     * to just above it, where the output is held.
     */
    int half  = level >> 1;
    int slope = (level - half) / (upLen - 4);
    int rest  = (level - half) - ((upLen - 4) * slope);

    shape->up[0] = 0;
    shape->up[1] = half - (level >> 3) + rest;
    for(int i = 2; i < upLen; i++)
    {
        int value = half + rest + ((i - 2) * slope);
        shape->up[i] = (value > APC_MAX) ? APC_MAX : value;
    }

    /*
     * Ramp down, stored last step first: four steep steps from the hold value
     * down to about half of it, then eight gentle ones down to zero.
     */
    int hold  = shape->up[upLen - 1];
    int quart = hold / 8;
    int base  = hold - (4 * quart);
    int fine  = (base + 4) / 8;

    memset(shape->down, 0, sizeof(shape->down));
    for(int k = 0; k < 4; k++)
    {
        int i = downLen - 4 + k;
        if(i >= 0)
            shape->down[i] = base + (k * quart);
    }
    for(int k = 0; k < 8; k++)
    {
        int i = downLen - 12 + k;
        if(i >= 0)
            shape->down[i] = k * fine;
    }

    size_t onLen  = sizeof(PA_ON) / sizeof(PA_ON[0]);
    size_t offLen = sizeof(PA_OFF) / sizeof(PA_OFF[0]);
    shape->paOn   = lookup(PA_ON, onLen, level);
    shape->paOff  = lookup(PA_OFF, offLen, level);

    return true;
}

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
