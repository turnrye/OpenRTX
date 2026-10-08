/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Fakes behind the DMR driver harness: the chip on its bus, the interrupt
 * lines, the payload bus, the audio, the debug log and time.
 */

#include "harness.h"
#include "hwconfig.h"
#include "peripherals/spi.h"
#include "peripherals/gpio.h"
#include "interfaces/delays.h"
#include "core/dmr_debug.h"
#include "drivers/baseband/dmr.h"
#include "drivers/baseband/dmrIrq.h"
#include "drivers/baseband/dmrPayload.h"
#include "drivers/baseband/dmr_audio.h"
#include <pthread.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include <condition_variable>

using namespace harness;

/* ---------------------------------------------------------------- time */

static std::atomic<long long> clockMs{ 0 };

long long getTick()
{
    return clockMs.load();
}

void delayUs(unsigned int useconds)
{
    (void)useconds;
}

void delayMs(unsigned int mseconds)
{
    clockMs += mseconds;
}

void sleepFor(unsigned int seconds, unsigned int mseconds)
{
    clockMs += seconds * 1000 + mseconds;
    sched_yield();
}

/* ---------------------------------------------------------------- gpio */

static bool fakeGpioRead(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev;
    (void)pin;
    return true;
}
static void fakeGpioSet(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev;
    (void)pin;
}
static void fakeGpioClear(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev;
    (void)pin;
}
static int fakeGpioMode(const struct gpioDev *dev, const uint8_t pin,
                        const uint16_t mode)
{
    (void)dev;
    (void)pin;
    (void)mode;
    return 0;
}

static const struct gpioApi fakeGpioApi = { fakeGpioMode, fakeGpioSet,
                                            fakeGpioClear, fakeGpioRead };
static const struct gpioDev fakeGpioDev = { &fakeGpioApi, NULL };

extern "C" uint8_t gpio_readPin(const void *port, const uint8_t pin)
{
    (void)port;
    (void)pin;
    return 1; // Interrupt lines idle high
}

/* ---------------------------------------------------------------- chip */

static std::mutex chipMutex;
static uint8_t regs[8][1024];
static std::vector<Write> writeLog;

/*
 * A transaction is a command, page and address, then data, the address
 * counting up with every byte. The chip class sends the command and the data
 * in one transfer for register accesses, and in two for page reads: the
 * second has no transmit buffer and continues where the command left off.
 */
static uint8_t curPage;
static uint16_t curAddr;
static bool curRead;

static int chipTransfer(const struct spiDevice *dev, const void *txBuf,
                        void *rxBuf, const size_t size)
{
    (void)dev;
    const uint8_t *tx = (const uint8_t *)txBuf;
    uint8_t *rx = (uint8_t *)rxBuf;
    std::lock_guard<std::mutex> lk(chipMutex);

    size_t first = 0;
    if (tx != NULL) {
        if (size < 2)
            return 0;
        curRead = (tx[0] & 0x80) != 0;
        bool ext = (tx[0] & 0x40) != 0;
        curPage = tx[0] & 0x0F;
        curAddr = tx[1];
        first = 2;
        if (ext && (size >= 3)) {
            curAddr = tx[1] | ((tx[2] & 0x07) << 8);
            first = 3;
        }
    }

    for (size_t i = first; i < size; i++) {
        uint16_t a = curAddr & 0x3FF;
        curAddr += 1;
        if (curRead || (tx == NULL)) {
            if (rx != NULL)
                rx[i] = regs[curPage & 7][a];
        } else {
            uint8_t v = tx[i];
            /* Writing a flag's bit to 0x83 acknowledges it in 0x82. */
            if ((curPage == 4) && (a == 0x83))
                regs[4][0x82] &= ~v;
            else
                regs[curPage & 7][a] = v;
            writeLog.push_back({ curPage, a, v, clockMs.load() });
        }
    }

    return 0;
}

static pthread_mutex_t chipBusMutex = PTHREAD_MUTEX_INITIALIZER;
static const struct spiDevice chipSpi = { chipTransfer, NULL, &chipBusMutex };
HR_C6000 C6000(&chipSpi, { &fakeGpioDev, 0 });

void harness::setReg(uint8_t page, uint16_t addr, uint8_t value)
{
    std::lock_guard<std::mutex> lk(chipMutex);
    regs[page & 7][addr & 0x3FF] = value;
}

uint8_t harness::getReg(uint8_t page, uint16_t addr)
{
    std::lock_guard<std::mutex> lk(chipMutex);
    return regs[page & 7][addr & 0x3FF];
}

void harness::raiseFlags(uint8_t flags)
{
    std::lock_guard<std::mutex> lk(chipMutex);
    regs[4][0x82] |= flags;
}

void harness::setLinkControl(const std::vector<uint8_t> &lc)
{
    std::lock_guard<std::mutex> lk(chipMutex);
    for (size_t i = 0; i < lc.size() && i < 12; i++)
        regs[2][i] = lc[i];
}

void harness::setPayload(const std::vector<uint8_t> &payload)
{
    std::lock_guard<std::mutex> lk(chipMutex);
    for (size_t i = 0; i < payload.size() && i < 27; i++)
        regs[2][0x66 + i] = payload[i];
}

std::vector<Write> harness::writes()
{
    std::lock_guard<std::mutex> lk(chipMutex);
    return writeLog;
}

std::vector<Write> harness::writes(uint8_t page, uint16_t addr)
{
    std::vector<Write> out;
    for (const auto &w : writes())
        if ((w.page == page) && (w.addr == addr))
            out.push_back(w);
    return out;
}

void harness::clearWrites()
{
    std::lock_guard<std::mutex> lk(chipMutex);
    writeLog.clear();
}

/* ---------------------------------------------------------------- irq */

static std::mutex irqMutex;
static std::condition_variable irqCond;
static uint32_t pendingEvents;
static bool waiting; // Driver thread blocked waiting
static struct dmrIrqCounters counters;

extern "C" {

void dmrIrq_init()
{
}
void dmrIrq_terminate()
{
}
void dmrIrq_getCounters(struct dmrIrqCounters *c)
{
    *c = counters;
}
void dmrIrq_clearCounters()
{
    memset(&counters, 0, sizeof(counters));
}
void dmrIrq_feedWatchdog()
{
}
bool dmrIrq_stormDetected()
{
    return false;
}
uint8_t dmrIrq_getRxColorCode()
{
    return 0xFF;
}
uint8_t dmrIrq_getRxBurstStatus()
{
    return 0;
}
void dmrIrq_setBusBusy(bool busy)
{
    (void)busy;
}

uint32_t dmrIrq_waitEvents()
{
    std::unique_lock<std::mutex> lk(irqMutex);
    waiting = true;
    irqCond.notify_all();
    irqCond.wait(lk, [] { return pendingEvents != 0; });
    waiting = false;
    uint32_t ev = pendingEvents;
    pendingEvents = 0;
    return ev;
}

uint32_t dmrIrq_takeEvents()
{
    std::lock_guard<std::mutex> lk(irqMutex);
    uint32_t ev = pendingEvents;
    pendingEvents = 0;
    return ev;
}

void dmrIrq_wakeWaiter()
{
    std::lock_guard<std::mutex> lk(irqMutex);
    pendingEvents |= DMR_IRQ_EVENT_WAKE;
    irqCond.notify_all();
}
}

static void post(uint32_t ev)
{
    std::lock_guard<std::mutex> lk(irqMutex);
    pendingEvents |= ev;
    irqCond.notify_all();
}

void harness::waitIdle()
{
    std::unique_lock<std::mutex> lk(irqMutex);
    irqCond.wait(lk, [] { return waiting && (pendingEvents == 0); });
}

void harness::timeslot()
{
    counters.timeslot += 1;
    post(DMR_IRQ_EVENT_TIMESLOT);
    waitIdle();
}

void harness::system()
{
    counters.system += 1;
    post(DMR_IRQ_EVENT_SYSTEM);
    waitIdle();
}

/* ---------------------------------------------------------------- payload bus */

static std::vector<std::vector<uint8_t>> payloadLog;

extern "C" {
void dmrPayload_init()
{
}
void dmrPayload_terminate()
{
}
void dmrPayload_read(uint8_t *payload)
{
    C6000.readDmrVoicePayload(payload);
}
void dmrPayload_write(const uint8_t *payload)
{
    std::lock_guard<std::mutex> lk(chipMutex);
    payloadLog.push_back(std::vector<uint8_t>(payload, payload + 27));
}
}

std::vector<std::vector<uint8_t>> harness::payloadsWritten()
{
    std::lock_guard<std::mutex> lk(chipMutex);
    return payloadLog;
}

/* ---------------------------------------------------------------- audio */

static std::atomic<int> rxStarts{ 0 }, rxStops{ 0 }, framesPushed{ 0 },
    txStarts{ 0 };
static std::atomic<bool> rxRunning{ false }, txRunning{ false };

extern "C" {
bool dmrAudio_startRx(void)
{
    rxStarts++;
    rxRunning = true;
    return true;
}
void dmrAudio_stopRx(void)
{
    if (rxRunning)
        rxStops++;
    rxRunning = false;
}
bool dmrAudio_running(void)
{
    return rxRunning;
}
void dmrAudio_pushFrame(const uint8_t *frame)
{
    (void)frame;
    framesPushed++;
}
bool dmrAudio_startTx(void)
{
    txStarts++;
    txRunning = true;
    return true;
}
void dmrAudio_stopTx(void)
{
    txRunning = false;
}
bool dmrAudio_txRunning(void)
{
    return txRunning;
}
bool dmrAudio_popFrame(uint8_t *frame)
{
    (void)frame;
    return false;
}
}

int harness::audioRxStarts()
{
    return rxStarts;
}
int harness::audioRxStops()
{
    return rxStops;
}
int harness::audioFramesPushed()
{
    return framesPushed;
}
int harness::audioTxStarts()
{
    return txStarts;
}

/* ---------------------------------------------------------------- debug log */

static std::mutex logMutex;
static std::vector<std::string> lines;

extern "C" {
void dmrDebug_init()
{
}
void dmrDebug_terminate()
{
}
void dmrDebug_task()
{
}
void dmrDebug_enableChannels(const uint8_t channels, const bool enable)
{
    (void)channels;
    (void)enable;
}
bool dmrDebug_channelEnabled(const enum dmrDebugChannel channel)
{
    (void)channel;
    return false;
}
void dmrDebug_mark(const char *msg)
{
    std::lock_guard<std::mutex> lk(logMutex);
    lines.push_back(msg);
}
void dmrDebug_record(const enum dmrDebugChannel channel, const char *tag,
                     const void *data, const size_t len)
{
    (void)channel;
    (void)tag;
    (void)data;
    (void)len;
}
void dmrDebug_log(const enum dmrDebugChannel channel, const char *fmt, ...)
{
    (void)channel;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(logMutex);
    lines.push_back(buf);
}
}

std::vector<std::string> harness::logLines()
{
    std::lock_guard<std::mutex> lk(logMutex);
    return lines;
}

/* ---------------------------------------------------------------- control */

void harness::reset()
{
    {
        std::lock_guard<std::mutex> lk(chipMutex);
        memset(regs, 0, sizeof(regs));
        writeLog.clear();
        payloadLog.clear();
    }
    {
        std::lock_guard<std::mutex> lk(logMutex);
        lines.clear();
    }
    clockMs = 0;
    rxStarts = rxStops = framesPushed = txStarts = 0;
    rxRunning = txRunning = false;
    memset(&counters, 0, sizeof(counters));
    pendingEvents = 0;
}

void harness::start(uint8_t colorCode)
{
    dmr_init(colorCode);
    waitIdle();
}

void harness::stop()
{
    dmr_terminate();
}

long long harness::now()
{
    return clockMs;
}
void harness::advance(long long ms)
{
    clockMs += ms;
}
void harness::setTime(long long ms)
{
    long long c = clockMs;
    if (ms > c)
        clockMs = ms;
}

RxState harness::poll()
{
    struct dmrRxState rx;
    dmr_task(&rx);
    return { rx.srcId, rx.dstId, rx.colorCode, rx.groupCall, rx.valid };
}

void harness::keyUp(bool group, uint32_t dst, uint32_t src)
{
    dmr_startTx(group, dst, src);
    waitIdle();
}

void harness::unkey()
{
    dmr_stopTx();
}

bool harness::txActive()
{
    return dmr_txActive();
}

void harness::txTask()
{
    dmr_txTask();
}

void harness::burst(uint8_t status, uint8_t colorCode, uint8_t flags)
{
    setReg(4, 0x51, status);
    setReg(4, 0x52, colorCode << 4);
    raiseFlags(flags);
    system();
}
