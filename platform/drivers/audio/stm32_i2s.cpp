/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <kernel/scheduler/scheduler.h>
#include "peripherals/gpio.h"
#include "interfaces/delays.h"
#include "hwconfig.h"
#include <miosix.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "stm32_i2s.h"

/*
 * The DMA fills a small ring of interleaved left/right halfwords; its half and
 * full interrupts copy the selected channel into the stream buffer. The stream
 * buffer can then be of any length, in both buffer modes.
 */

static constexpr size_t HALF_FRAMES = 80;                   // 10 ms at 8 kHz
static constexpr size_t RING_WORDS  = 2 * HALF_FRAMES * 2;  // Two halves, L+R
static constexpr uint32_t WS_POLLS  = 200000;               // Polls before "no clock"

// DMA can't reach the CCM, where .bss is on this MCU: the ring is on the heap
static int16_t *ring;

static struct streamCtx *volatile ctx;
static miosix::Thread  *volatile waiting;
static volatile size_t   pos;
static volatile uint8_t  readyHalf;
static volatile bool     event;
static volatile bool     stopReq;
static volatile bool     useRight;

static volatile uint32_t statFrames;
static volatile int16_t  statPeak[2];
static volatile int64_t  statSum[2];
static volatile uint64_t statSumSq[2];
static volatile uint32_t statNoClock;

static volatile uint32_t dIrqs, dHt, dTc, dTe, dStarts, dStartFails;
static volatile uint32_t dTimeouts;

/**
 * Stop the DMA stream and the I2S peripheral.
 */
static void hwStop()
{
    SPI3->I2SCFGR &= ~SPI_I2SCFGR_I2SE;
    DMA1_Stream0->CR &= ~DMA_SxCR_EN;
    while((DMA1_Stream0->CR & DMA_SxCR_EN) != 0) ;

    DMA1->LIFCR = DMA_LIFCR_CFEIF0 | DMA_LIFCR_CDMEIF0 | DMA_LIFCR_CTEIF0
                | DMA_LIFCR_CHTIF0 | DMA_LIFCR_CTCIF0;
}

/**
 * Wait for a level on the WS line.
 *
 * @param level: the level to wait for.
 * @return false if WS did not reach the level within WS_POLLS polls.
 */
static bool wsWait(const uint8_t level)
{
    for(uint32_t i = 0; i < WS_POLLS; i++)
    {
        if(gpio_readPin(I2S3_WS) == level)
            return true;
    }

    return false;
}

/**
 * Start the DMA ring and enable the I2S slave just after a WS rising edge,
 * so that the first frame received starts with the left channel.
 *
 * @return false if the codec drives no WS clock.
 */
static bool hwStart()
{
    hwStop();

    // Drop stale data and a pending overrun (read DR, then SR)
    (void) SPI3->DR;
    (void) SPI3->SR;

    DMA1_Stream0->PAR  = (uint32_t) &SPI3->DR;
    DMA1_Stream0->M0AR = (uint32_t) ring;
    DMA1_Stream0->NDTR = RING_WORDS;
    DMA1_Stream0->FCR  = 0;
    DMA1_Stream0->CR   = (0u << DMA_SxCR_CHSEL_Pos)  // Channel 0: SPI3_RX
                       | DMA_SxCR_PL_1               // High priority
                       | DMA_SxCR_MSIZE_0            // 16 bit memory
                       | DMA_SxCR_PSIZE_0            // 16 bit peripheral
                       | DMA_SxCR_MINC
                       | DMA_SxCR_CIRC
                       | DMA_SxCR_HTIE
                       | DMA_SxCR_TCIE
                       | DMA_SxCR_TEIE;
    DMA1_Stream0->CR  |= DMA_SxCR_EN;

    if(wsWait(1) == false)
    {
        hwStop();
        return false;
    }

    bool ok;
    {
        miosix::FastInterruptDisableLock dLock;
        ok = wsWait(0) && wsWait(1);
        if(ok)
            SPI3->I2SCFGR |= SPI_I2SCFGR_I2SE;
    }

    if(ok == false)
        hwStop();

    return ok;
}

/**
 * End the stream: called from the DMA interrupt or with interrupts disabled.
 */
static void IRQendStream()
{
    hwStop();
    stopReq = false;
    if(ctx != nullptr)
        ctx->running = 0;
}

/**
 * Wake the thread waiting in sync(), if any.
 */
static void IRQwakeWaiting()
{
    event = true;

    miosix::Thread *t = waiting;
    if(t == nullptr)
        return;

    t->IRQwakeup();
    if(t->IRQgetPriority() > miosix::Thread::IRQgetCurrentThread()->IRQgetPriority())
        miosix::Scheduler::IRQfindNextThread();

    waiting = nullptr;
}

/**
 * Store one sample into the stream buffer and signal buffer boundaries.
 *
 * @param s: sample.
 */
static void IRQput(const int16_t s)
{
    struct streamCtx *c = ctx;
    if((c == nullptr) || (c->running == 0))
        return;

    c->buffer[pos] = s;
    pos = pos + 1;

    if(c->bufMode == BUF_LINEAR)
    {
        if(pos >= c->bufSize)
        {
            IRQendStream();
            IRQwakeWaiting();
        }

        return;
    }

    const size_t half = c->bufSize / 2;
    if((pos != half) && (pos < c->bufSize))
        return;

    if(pos >= c->bufSize)
    {
        pos       = 0;
        readyHalf = 1;
    }
    else
    {
        readyHalf = 0;
    }

    if(stopReq)
        IRQendStream();

    IRQwakeWaiting();
}

/**
 * Process one half of the DMA ring: levels of both channels, then the
 * selected channel into the stream.
 *
 * @param half: ring half, 0 or 1.
 */
static void IRQprocess(const uint8_t half)
{
    const int16_t *in = &ring[half * HALF_FRAMES * 2];

    for(size_t f = 0; f < HALF_FRAMES; f++)
    {
        const int16_t s[2] = { in[2 * f], in[2 * f + 1] };

        for(uint8_t ch = 0; ch < 2; ch++)
        {
            int16_t a = (s[ch] < 0) ? ((s[ch] == INT16_MIN) ? INT16_MAX : -s[ch]) : s[ch];
            if(a > statPeak[ch])
                statPeak[ch] = a;

            statSum[ch]   = statSum[ch] + s[ch];
            statSumSq[ch] = statSumSq[ch] + (uint64_t) ((int32_t) s[ch] * s[ch]);
        }

        statFrames = statFrames + 1;
        IRQput(useRight ? s[1] : s[0]);
    }
}

/**
 * DMA1 Stream 0 interrupt body.
 */
void __attribute__((used)) I2S_DMA_Handler()
{
    const uint32_t flags = DMA1->LISR & (DMA_LISR_HTIF0 | DMA_LISR_TCIF0
                                       | DMA_LISR_TEIF0 | DMA_LISR_DMEIF0
                                       | DMA_LISR_FEIF0);
    DMA1->LIFCR = flags;

    dIrqs = dIrqs + 1;
    if((flags & DMA_LISR_HTIF0) != 0) dHt = dHt + 1;
    if((flags & DMA_LISR_TCIF0) != 0) dTc = dTc + 1;
    if((flags & DMA_LISR_TEIF0) != 0) dTe = dTe + 1;

    if((flags & DMA_LISR_HTIF0) != 0)
        IRQprocess(0);

    if((flags & DMA_LISR_TCIF0) != 0)
        IRQprocess(1);
}

// DMA 1, Stream 0: SPI3_RX
void __attribute__((used)) DMA1_Stream0_IRQHandler()
{
    saveContext();
    asm volatile("bl _Z15I2S_DMA_Handlerv");
    restoreContext();
}

void stm32i2s_init()
{
    if(ring == nullptr)
        ring = (int16_t *) malloc(RING_WORDS * sizeof(int16_t));

    RCC->APB1ENR |= RCC_APB1ENR_SPI3EN;
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;
    __DSB();

    gpio_setMode(I2S3_WS, ALTERNATE | ALTERNATE_FUNC(6));
    gpio_setMode(I2S3_CK, ALTERNATE | ALTERNATE_FUNC(6));
    gpio_setMode(I2S3_SD, ALTERNATE | ALTERNATE_FUNC(6));

    // Philips standard, 16-bit data in a 32-bit channel, clock idle high,
    // slave receive
    SPI3->I2SCFGR = 0;
    SPI3->I2SPR   = 2;
    SPI3->I2SCFGR = SPI_I2SCFGR_I2SMOD
                  | SPI_I2SCFGR_CHLEN
                  | SPI_I2SCFGR_CKPOL
                  | SPI_I2SCFGR_I2SCFG_0;
    SPI3->CR2     = SPI_CR2_RXDMAEN;

    NVIC_ClearPendingIRQ(DMA1_Stream0_IRQn);
    NVIC_SetPriority(DMA1_Stream0_IRQn, 10);
    NVIC_EnableIRQ(DMA1_Stream0_IRQn);
}

void stm32i2s_terminate()
{
    {
        miosix::FastInterruptDisableLock dLock;
        IRQendStream();
        IRQwakeWaiting();
    }

    NVIC_DisableIRQ(DMA1_Stream0_IRQn);
    RCC->APB1ENR &= ~RCC_APB1ENR_SPI3EN;
    __DSB();
}

void stm32i2s_setChannel(const bool right)
{
    useRight = right;
}

void stm32i2s_getStats(struct i2sStats *stats, const bool reset)
{
    uint32_t frames;
    int16_t  peak[2];
    int64_t  sum[2];
    uint64_t sumSq[2];

    {
        miosix::FastInterruptDisableLock dLock;
        frames = statFrames;
        for(uint8_t ch = 0; ch < 2; ch++)
        {
            peak[ch]  = statPeak[ch];
            sum[ch]   = statSum[ch];
            sumSq[ch] = statSumSq[ch];
        }

        stats->noClock = statNoClock;

        if(reset)
        {
            statFrames = 0;
            for(uint8_t ch = 0; ch < 2; ch++)
            {
                statPeak[ch]  = 0;
                statSum[ch]   = 0;
                statSumSq[ch] = 0;
            }
        }
    }

    stats->frames = frames;
    for(uint8_t ch = 0; ch < 2; ch++)
    {
        float mean = 0.0f;
        float var  = 0.0f;
        if(frames > 0)
        {
            mean = (float) sum[ch] / (float) frames;
            var  = ((float) sumSq[ch] / (float) frames) - (mean * mean);
        }

        stats->peak[ch] = peak[ch];
        stats->mean[ch] = (int16_t) mean;
        stats->rms[ch]  = (var > 0.0f) ? (uint16_t) sqrtf(var) : 0;
    }
}

void stm32i2s_getDiag(struct i2sDiag *d)
{
    d->irqs       = dIrqs;
    d->ht         = dHt;
    d->tc         = dTc;
    d->te         = dTe;
    d->starts     = dStarts;
    d->startFails = dStartFails;
    d->timeouts   = dTimeouts;
    d->pos        = pos;
    d->running    = (ctx != nullptr) ? ctx->running : 0xFF;
    d->sr         = SPI3->SR;
    d->i2scfgr    = SPI3->I2SCFGR;
    d->dmaCr      = DMA1_Stream0->CR;
    d->ndtr       = DMA1_Stream0->NDTR;
    d->lisr       = DMA1->LISR;

    // WS = PA15, CK = PC10, SD = PC12, read straight from the pins over 1 ms
    uint32_t ws = 0, ck = 0, sd = 0, n = 0;
    bool lw = (GPIOA->IDR & (1u << 15)) != 0;
    bool lc = (GPIOC->IDR & (1u << 10)) != 0;

    long long t = getTick();
    while(getTick() == t) ;
    t = getTick();
    while(getTick() == t)
    {
        const uint32_t pa = GPIOA->IDR;
        const uint32_t pc = GPIOC->IDR;
        const bool w = (pa & (1u << 15)) != 0;
        const bool c = (pc & (1u << 10)) != 0;
        if(w != lw) { ws++; lw = w; }
        if(c != lc) { ck++; lc = c; }
        if((pc & (1u << 12)) != 0) sd++;
        n++;
    }

    d->wsEdges = ws;
    d->ckEdges = ck;
    d->sdHigh  = sd;
    d->polls   = n;
}

static int stm32i2s_start(const uint8_t instance, const void *config,
                          struct streamCtx *c)
{
    (void) instance;
    (void) config;

    if(c == nullptr)
        return -EINVAL;

    if(c->running != 0)
        return -EBUSY;

    if(c->sampleRate != 8000)
        return -EINVAL;

    if(ring == nullptr)
        return -ENOMEM;

    if((c->bufMode == BUF_CIRC_DOUBLE) && ((c->bufSize % 2) != 0))
        return -EINVAL;

    {
        miosix::FastInterruptDisableLock dLock;
        ctx       = c;
        pos       = 0;
        readyHalf = 0;
        event     = false;
        stopReq   = false;
        c->running = 1;
    }

    dStarts = dStarts + 1;
    if(hwStart() == false)
    {
        dStartFails = dStartFails + 1;
        c->running  = 0;
        statNoClock = statNoClock + 1;
        return -EIO;
    }

    return 0;
}

static int stm32i2s_data(struct streamCtx *c, stream_sample_t **buf)
{
    if(c->bufMode == BUF_LINEAR)
    {
        *buf = c->buffer;
        return c->bufSize;
    }

    const size_t half = c->bufSize / 2;
    *buf = &c->buffer[readyHalf * half];
    return half;
}

static int stm32i2s_sync(struct streamCtx *c, uint8_t dirty)
{
    (void) dirty;

    // Bench version: poll with a timeout, so that a stalled stream shows up
    // in the diagnostics instead of blocking the caller for ever
    for(uint32_t i = 0; i < 200; i++)
    {
        if(event)
        {
            event = false;
            return 0;
        }

        if(c->running == 0)
            return -1;

        sleepFor(0, 1);
    }

    dTimeouts = dTimeouts + 1;
    return -1;
}

static void stm32i2s_stop(struct streamCtx *c)
{
    if(c->running == 0)
        return;

    stopReq = true;
}

static void stm32i2s_halt(struct streamCtx *c)
{
    if(c->running == 0)
        return;

    miosix::FastInterruptDisableLock dLock;
    IRQendStream();
    IRQwakeWaiting();
}

#pragma GCC diagnostic ignored "-Wpedantic"
const struct audioDriver stm32_i2s_audio_driver =
{
    .start     = stm32i2s_start,
    .data      = stm32i2s_data,
    .sync      = stm32i2s_sync,
    .stop      = stm32i2s_stop,
    .terminate = stm32i2s_halt
};
#pragma GCC diagnostic pop
