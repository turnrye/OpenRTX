/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "peripherals/spi.h"
#include "hwconfig.h"
#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "interfaces/delays.h"
#include "usb_vcom.h"
#include "i2s_parrot.h"

#define SAMPLE_RATE   8000
#define REC_SAMPLES   (3 * SAMPLE_RATE)
#define BLOCK_SAMPLES 160           // 20 ms per stream half
#define MAX_GAIN      16
#define PLAY_PEAK     16000         // Playback peak after the gain
#define SWITCH_RATIO  4             // Other channel this much louder: use it
#define SWITCH_MIN    30            // ... and above this RMS

static stream_sample_t rec[REC_SAMPLES];
static stream_sample_t block[2 * BLOCK_SAMPLES];
static struct parrotStatus status;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t serMutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * Write to the USB serial port, in pieces the CDC endpoint takes.
 * Caller holds serMutex.
 *
 * @return false if the host did not take the data (port not open).
 */
static bool serWrite(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *) buf;
    while(len > 0)
    {
        size_t n = (len > 512) ? 512 : len;
        if(vcom_writeBlock(p, n) != (ssize_t) n)
            return false;

        p   += n;
        len -= n;
    }

    return true;
}

/**
 * printf-style line to the USB serial port.
 */
static void serLog(const char *fmt, ...)
{
    char line[320];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);

    if(n < 0)
        return;
    if((size_t) n > sizeof(line) - 3)
        n = sizeof(line) - 3;

    line[n]     = '\r';
    line[n + 1] = '\n';

    pthread_mutex_lock(&serMutex);
    serWrite(line, n + 2);
    pthread_mutex_unlock(&serMutex);
}

/**
 * Dump a take as base64 between "PCM" and "END" lines, 48 bytes a line.
 * Stops at the first write the host does not take.
 */
static void serDump(const uint32_t take, const stream_sample_t *pcm, const size_t n)
{
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char hdr[48];
    char line[66];
    const uint8_t *src = (const uint8_t *) pcm;    // Little endian s16
    const size_t bytes = n * sizeof(stream_sample_t);

    int h = snprintf(hdr, sizeof(hdr), "PCM %lu %u 8000 s16le\r\n",
                     (unsigned long) take, (unsigned) n);

    pthread_mutex_lock(&serMutex);
    bool ok = serWrite(hdr, h);

    for(size_t off = 0; ok && (off < bytes); off += 48)
    {
        size_t chunk = ((bytes - off) > 48) ? 48 : (bytes - off);
        size_t o = 0;

        for(size_t i = 0; i < chunk; i += 3)
        {
            uint32_t v = (uint32_t) src[off + i] << 16;
            if((i + 1) < chunk) v |= (uint32_t) src[off + i + 1] << 8;
            if((i + 2) < chunk) v |= (uint32_t) src[off + i + 2];

            line[o++] = b64[(v >> 18) & 0x3F];
            line[o++] = b64[(v >> 12) & 0x3F];
            line[o++] = ((i + 1) < chunk) ? b64[(v >> 6) & 0x3F] : '=';
            line[o++] = ((i + 2) < chunk) ? b64[v & 0x3F] : '=';
        }

        line[o++] = '\r';
        line[o++] = '\n';
        ok = serWrite(line, o);
    }

    if(ok)
    {
        h = snprintf(hdr, sizeof(hdr), "END %lu\r\n", (unsigned long) take);
        serWrite(hdr, h);
    }

    pthread_mutex_unlock(&serMutex);
}

/**
 * Update the shared phase and error.
 */
static void setPhase(const uint8_t phase, const int16_t error)
{
    pthread_mutex_lock(&mutex);
    status.phase = phase;
    status.error = error;
    pthread_mutex_unlock(&mutex);
}

/**
 * Record up to REC_SAMPLES from the microphone.
 *
 * @return samples recorded, negative error code on failure.
 */
static int record(size_t want)
{
    pathId path = audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX);
    if(path < 0)
        return -1;

    streamId id = audioStream_start(path, block, 2 * BLOCK_SAMPLES, SAMPLE_RATE,
                                    STREAM_INPUT | BUF_CIRC_DOUBLE);
    if(id < 0)
    {
        serLog("REC start failed %d", id);
        audioPath_release(path);
        return -2;
    }

    size_t n = 0;
    if(want > REC_SAMPLES)
        want = REC_SAMPLES;

    while(n < want)
    {
        dataBlock_t b = inputStream_getData(id);
        if((b.data == NULL) || (b.len == 0))
        {
            serLog("REC getData empty at %u", (unsigned) n);
            break;
        }

        size_t len = b.len;
        if(len > (want - n))
            len = want - n;

        memcpy(&rec[n], b.data, len * sizeof(stream_sample_t));
        n += len;
    }

    audioStream_terminate(id);
    audioPath_release(path);

    return (int) n;
}

/**
 * Remove the mean of the take and scale it up to PLAY_PEAK, at most MAX_GAIN.
 *
 * @param n: samples in the take.
 * @return gain applied.
 */
static uint8_t normalise(const size_t n)
{
    if(n == 0)
        return 1;

    int32_t sum = 0;
    for(size_t i = 0; i < n; i++)
        sum += rec[i];

    const int32_t mean = sum / (int32_t) n;
    int32_t peak = 0;
    for(size_t i = 0; i < n; i++)
    {
        int32_t a = rec[i] - mean;
        if(a < 0)
            a = -a;
        if(a > peak)
            peak = a;
    }

    int32_t gain = (peak > 0) ? (PLAY_PEAK / peak) : 1;
    if(gain < 1)
        gain = 1;
    if(gain > MAX_GAIN)
        gain = MAX_GAIN;

    for(size_t i = 0; i < n; i++)
    {
        int32_t s = (rec[i] - mean) * gain;
        if(s > INT16_MAX)
            s = INT16_MAX;
        if(s < INT16_MIN)
            s = INT16_MIN;
        rec[i] = (stream_sample_t) s;
    }

    return (uint8_t) gain;
}

/**
 * Play the take on the speaker, blocking until done.
 *
 * @param n: samples in the take.
 * @return 0 on success, negative error code on failure.
 */
static int play(const size_t n)
{
    long long t0 = getTick();

    pathId path = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_PROMPT);
    if(path < 0)
    {
        serLog("PLAY path request failed %ld", (long) path);
        return -3;
    }

    // Circular double buffer, as the Codec2 decoder does: audio_stream never
    // starts a linear output stream
    memset(block, 0, sizeof(block));
    streamId id = audioStream_start(path, block, 2 * BLOCK_SAMPLES, SAMPLE_RATE,
                                    STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    if(id < 0)
    {
        serLog("PLAY stream start failed %d (path %ld)", id, (long) path);
        audioPath_release(path);
        return -4;
    }

    long long t1 = getTick();
    outputStream_sync(id, false);

    size_t done = 0;
    uint32_t syncFails = 0;
    while(done < n)
    {
        stream_sample_t *buf = outputStream_getIdleBuffer(id);
        if(buf == NULL)
            break;

        size_t len = n - done;
        if(len > BLOCK_SAMPLES)
            len = BLOCK_SAMPLES;

        memcpy(buf, &rec[done], len * sizeof(stream_sample_t));
        if(len < BLOCK_SAMPLES)
            memset(&buf[len], 0, (BLOCK_SAMPLES - len) * sizeof(stream_sample_t));

        done += len;
        if(outputStream_sync(id, true) == false)
            syncFails++;
    }

    audioStream_stop(id);
    long long t2 = getTick();
    audioPath_release(path);

    serLog("PLAY path %ld stream %d: %u of %u samples, %ld ms, %lu sync fails",
           (long) path, id, (unsigned) done, (unsigned) n, (long) (t2 - t1),
           (unsigned long) syncFails);
    (void) t0;

    return 0;
}

void i2sParrot_writeC6000(const uint8_t reg, const uint8_t val);
void i2sParrot_readC6000(const uint8_t *regs, uint8_t *vals, size_t n);
int  i2sParrot_c6000Op(const char *op, const int arg);

static size_t   recLen;         // Samples in rec
static bool     monitor;        // Diagnostics line every second
static uint32_t takeNum;

/**
 * Careful read of an HR_C6000 register over its bit-banged port, for
 * comparison with the driver's fast read. MOSI changes while CLK is high and
 * the chip latches it on the falling edge, as in spiC6000_func.
 *
 * @param page: register page (4 = configuration).
 * @param reg: register.
 * @param mode: 0 sample MISO after the falling edge, 1 just before it,
 *              2 after the rising edge.
 * @param us: half clock period in microseconds.
 */
static uint8_t slowRead(const uint8_t page, const uint8_t reg, const int mode,
                        const uint32_t us)
{
    const uint8_t tx[3] = { (uint8_t) (0x80 | page), reg, 0x00 };
    uint8_t rx[3] = { 0, 0, 0 };

    spi_acquire((const struct spiDevice *) &c6000_spi);
    GPIOE->BSRR = 1u << (16 + 2);           // CS low (PE2)
    delayUs(2);

    for(int byte = 0; byte < 3; byte++)
    {
        uint8_t in = 0;
        for(int bit = 7; bit >= 0; bit--)
        {
            GPIOE->BSRR = 1u << 3;          // CLK high (PE3)
            if(mode == 2)
            {
                delayUs(us);
                in = (uint8_t) ((in << 1) | ((GPIOE->IDR >> 4) & 1u));
            }

            if(tx[byte] & (1u << bit))
                GPIOE->BSRR = 1u << 5;      // MOSI high (PE5)
            else
                GPIOE->BSRR = 1u << (16 + 5);

            delayUs(us);
            if(mode == 1)
                in = (uint8_t) ((in << 1) | ((GPIOE->IDR >> 4) & 1u));

            GPIOE->BSRR = 1u << (16 + 3);   // CLK low
            delayUs(us);
            if(mode == 0)
                in = (uint8_t) ((in << 1) | ((GPIOE->IDR >> 4) & 1u));
        }
        rx[byte] = in;
    }

    delayUs(2);
    GPIOE->BSRR = 1u << 2;                  // CS high
    delayUs(2);
    spi_release((const struct spiDevice *) &c6000_spi);

    return rx[2];
}

extern volatile uint32_t c6000ReadDelay;

/**
 * Careful byte transfer on the HR_C6000 port: MOSI set while CLK is high,
 * MISO sampled after the falling edge.
 */
static uint8_t slowXfer(const uint8_t out)
{
    uint8_t in = 0;
    for(int bit = 7; bit >= 0; bit--)
    {
        GPIOE->BSRR = 1u << 3;
        if(out & (1u << bit))
            GPIOE->BSRR = 1u << 5;
        else
            GPIOE->BSRR = 1u << (16 + 5);
        delayUs(0);
        GPIOE->BSRR = 1u << (16 + 3);
        delayUs(0);
        in = (uint8_t) ((in << 1) | ((GPIOE->IDR >> 4) & 1u));
    }

    return in;
}

/**
 * Read HR_C6000 RAM: page with 16-bit address, low byte first.
 */
static void ramRead(const uint8_t page, const uint16_t addr, uint8_t *buf,
                    const size_t n)
{
    spi_acquire((const struct spiDevice *) &c6000_spi);
    GPIOE->BSRR = 1u << (16 + 2);
    delayUs(2);

    slowXfer((uint8_t) (0x80 | 0x40 | page));
    slowXfer((uint8_t) (addr & 0xFF));
    slowXfer((uint8_t) ((addr >> 8) & 0x07));
    for(size_t i = 0; i < n; i++)
        buf[i] = slowXfer(0x00);

    delayUs(2);
    GPIOE->BSRR = 1u << 2;
    delayUs(2);
    spi_release((const struct spiDevice *) &c6000_spi);
}

#define COPY_RAM    0x030
#define COPY_HALF   512     // bytes: 256 big endian samples

/**
 * Last sample of a copy half.
 */
static int16_t copyLast(const int half)
{
    uint8_t b[2];
    ramRead(2, (uint16_t) (COPY_RAM + half * COPY_HALF + COPY_HALF - 2), b, 2);
    return (int16_t) ((b[0] << 8) | b[1]);
}

/**
 * Capture the HR_C6000 FM receive copy (its FM demodulator, 8 kHz) into rec,
 * with RadioFoundry's copy set-up (modem-hrc6000.md 10.6), then restore.
 */
static void cmdFmCap(const uint32_t ms, const bool vhfInvert)
{
    static const uint8_t saveRegs[] = { 0x41, 0x36, 0x60, 0x34, 0x10, 0x81 };
    uint8_t saved[sizeof(saveRegs)];
    uint8_t half[COPY_HALF];

    for(size_t i = 0; i < sizeof(saveRegs); i++)
        saved[i] = slowRead(4, saveRegs[i], 0, 0);

    i2sParrot_writeC6000(0x41, 0x00);
    i2sParrot_writeC6000(0x36, 0x00);
    i2sParrot_writeC6000(0x60, 0x00);
    i2sParrot_writeC6000(0x34, 0x00);
    i2sParrot_writeC6000(0x10, 0x80);
    i2sParrot_writeC6000(0x81, (uint8_t) (saved[5] | 0x02));
    i2sParrot_writeC6000(0x83, 0xFF);

    size_t want = (size_t) ms * (SAMPLE_RATE / 1000);
    if(want > REC_SAMPLES)
        want = REC_SAMPLES;

    int16_t last[2] = { copyLast(0), copyLast(1) };
    int lastHalf = 1;
    uint32_t halves = 0, fixes = 0, polls = 0;
    size_t n = 0;
    setPhase(PARROT_REC, 0);
    long long t0 = getTick();
    long long deadline = t0 + ms + 1000;

    while((n < want) && (getTick() < deadline))
    {
        polls++;
        uint8_t flags = slowRead(4, 0x82, 0, 0);
        if((flags & 0x02) == 0)
        {
            sleepFor(0, 2);
            continue;
        }

        i2sParrot_writeC6000(0x83, 0x02);

        const int16_t now[2] = { copyLast(0), copyLast(1) };
        const bool ch0 = (now[0] != last[0]);
        const bool ch1 = (now[1] != last[1]);
        int h;
        if(ch0 && !ch1)       h = 0;
        else if(ch1 && !ch0)  h = 1;
        else                  { h = lastHalf ^ 1; fixes++; }

        ramRead(2, (uint16_t) (COPY_RAM + h * COPY_HALF), half, COPY_HALF);
        for(size_t i = 0; (i < COPY_HALF / 2) && (n < want); i++)
        {
            int16_t v = (int16_t) ((half[2 * i] << 8) | half[2 * i + 1]);
            rec[n++] = vhfInvert ? (int16_t) -v : v;
        }

        last[0] = now[0];
        last[1] = now[1];
        lastHalf = h;
        halves++;
    }

    long long t1 = getTick();

    i2sParrot_writeC6000(0x10, saved[4]);
    i2sParrot_writeC6000(0x34, saved[3]);
    i2sParrot_writeC6000(0x60, saved[2]);
    i2sParrot_writeC6000(0x36, saved[1]);
    i2sParrot_writeC6000(0x81, saved[5]);
    i2sParrot_writeC6000(0x41, saved[0]);
    i2sParrot_writeC6000(0x83, 0xFF);
    setPhase(PARROT_IDLE, 0);

    int16_t pk = 0;
    int64_t sum = 0;
    for(size_t i = 0; i < n; i++)
    {
        int16_t a = (rec[i] < 0) ? (int16_t) -rec[i] : rec[i];
        if(a > pk) pk = a;
        sum += rec[i];
    }

    recLen = n;
    serLog("FMCAP n=%u %ld ms halves=%lu fixes=%lu polls=%lu peak=%d mean=%ld "
           "saved 41=%02x 36=%02x 60=%02x 34=%02x 10=%02x 81=%02x",
           (unsigned) n, (long) (t1 - t0), (unsigned long) halves,
           (unsigned long) fixes, (unsigned long) polls, pk,
           (long) ((n > 0) ? (sum / (int64_t) n) : 0), saved[0], saved[1],
           saved[2], saved[3], saved[4], saved[5]);

    if(n > 0)
        serDump(takeNum++, rec, n);
}

uint32_t radio_testTsCount();
void     radio_testTsIrq(const bool on);
int      radio_testPhyPrep();
void     radio_testPa(const bool on);
void     radio_testPhyEnd();
uint32_t radio_testTxDrive();

#define SYM_MAX     4096    // bytes of dibits: 16384 symbols, 3.4 s
#define SLOT_BYTES  36      // 144 symbols per 30 ms slot
#define IDLE_BYTE   0x77    // +3 -3 +3 -3: the M17 preamble

static uint8_t *sym;
static size_t   symLen;
static uint8_t  devMod1 = 0x65;     // RadioFoundry: 2400 Hz from the 0x52/0x2A pair
static uint8_t  devMod2 = 0x34;

/**
 * Write HR_C6000 RAM with 8-bit addressing (page 2 TX RAM).
 */
static void ramWrite(const uint8_t page, const uint8_t addr, const uint8_t *buf,
                     const size_t n)
{
    spi_acquire((const struct spiDevice *) &c6000_spi);
    GPIOE->BSRR = 1u << (16 + 2);
    delayUs(2);

    slowXfer(page);
    slowXfer(addr);
    for(size_t i = 0; i < n; i++)
        slowXfer(buf[i]);

    delayUs(2);
    GPIOE->BSRR = 1u << 2;
    delayUs(2);
    spi_release((const struct spiDevice *) &c6000_spi);
}

/**
 * Wait for the next HR_C6000 timeslot interrupt.
 *
 * @param last: count seen last, updated.
 * @return interrupts since last (0 = timed out after 200 ms).
 */
static uint32_t tsWait(uint32_t *last)
{
    for(int i = 0; i < 200; i++)
    {
        uint32_t now = radio_testTsCount();
        if(now != *last)
        {
            uint32_t d = now - *last;
            *last = now;
            return d;
        }
        sleepFor(0, 1);
    }
    return 0;
}

/**
 * Transmit the uploaded symbols in the HR_C6000 physical layer mode
 * (RadioFoundry modem-hrc6000.md 10.3), lowest PA row.
 */
static void cmdPhyTx(uint32_t maxSlots)
{
    static const uint8_t saveRegs[] = { 0x10, 0x40, 0x41, 0x20, 0x21, 0x22, 0x45, 0x46 };
    uint8_t saved[sizeof(saveRegs)];
    uint8_t chunk[SLOT_BYTES];

    if((sym == NULL) || (symLen == 0))
    {
        serLog("ERR no symbols, use sym");
        return;
    }

    if(maxSlots > 200)
        maxSlots = 200;

    const uint32_t chunks = (uint32_t) ((symLen + SLOT_BYTES - 1) / SLOT_BYTES);

    for(size_t i = 0; i < sizeof(saveRegs); i++)
        saved[i] = slowRead(4, saveRegs[i], 0, 0);

    int ret = radio_testPhyPrep();
    if(ret < 0)
    {
        serLog("ERR prep %d (not V5/no band = -1, supply = -2)", ret);
        radio_testPhyEnd();
        return;
    }

    setPhase(PARROT_PLAY, 0);
    i2sParrot_writeC6000(0x45, devMod2);
    i2sParrot_writeC6000(0x46, devMod1);
    i2sParrot_writeC6000(0x41, 0x00);
    i2sParrot_writeC6000(0x10, 0x66);
    i2sParrot_writeC6000(0x20, 0x00);
    i2sParrot_writeC6000(0x21, 0x02);
    i2sParrot_writeC6000(0x22, 0x86);

    memset(chunk, IDLE_BYTE, sizeof(chunk));
    ramWrite(2, 0x00, chunk, SLOT_BYTES);
    ramWrite(2, 0x24, chunk, SLOT_BYTES);

    radio_testTsIrq(true);
    uint32_t last = radio_testTsCount();
    long long t0 = getTick();
    i2sParrot_writeC6000(0x40, 0xE3);

    uint32_t n = 0, k = 0, jumps = 0, idleTail = 0;
    bool paOn = false, stall = false;
    long long tPa = 0, tEnd = 0;

    while(n < maxSlots)
    {
        uint32_t d = tsWait(&last);
        if(d == 0) { stall = true; break; }
        if(d > 1)  jumps++;
        n += d;

        // Slot n+1 is set up at interrupt n and reads 0x00 when n is odd
        const uint8_t half = (n & 1) ? 0x00 : 0x24;

        if(n == 2)
        {
            radio_testPa(true);
            paOn = true;
            tPa = getTick();
        }

        if((n >= 2) && (k < chunks))
        {
            size_t off = (size_t) k * SLOT_BYTES;
            size_t len = ((symLen - off) > SLOT_BYTES) ? SLOT_BYTES : (symLen - off);
            memcpy(chunk, &sym[off], len);
            if(len < SLOT_BYTES)
                memset(&chunk[len], IDLE_BYTE, SLOT_BYTES - len);
            k++;
        }
        else
        {
            memset(chunk, IDLE_BYTE, sizeof(chunk));
            if(k >= chunks)
                idleTail++;
        }

        ramWrite(2, half, chunk, SLOT_BYTES);
        i2sParrot_writeC6000(0x41, 0x80);

        // One idle slot after the last chunk, then drop at the next interrupt
        if(idleTail > 1)
            break;
    }

    if(paOn)
        radio_testPa(false);
    tEnd = getTick();

    radio_testTsIrq(false);
    i2sParrot_writeC6000(0x41, 0x00);
    for(size_t i = 0; i < sizeof(saveRegs); i++)
        i2sParrot_writeC6000(saveRegs[i], saved[i]);
    radio_testPhyEnd();
    setPhase(PARROT_IDLE, 0);

    serLog("PHYTX %lu/%lu chunks, %lu interrupts, %lu jumps%s, PA %ld ms after start, "
           "on %ld ms, drive %lu, dev %02x/%02x, saved 10=%02x 40=%02x 41=%02x 45=%02x 46=%02x",
           (unsigned long) k, (unsigned long) chunks, (unsigned long) n,
           (unsigned long) jumps, stall ? " STALLED" : "",
           (long) (tPa - t0), (long) (tEnd - tPa),
           (unsigned long) radio_testTxDrive(), devMod1, devMod2, saved[0],
           saved[1], saved[2], saved[6], saved[7]);
}

/**
 * One diagnostics line.
 */
static void logStat()
{
    struct i2sDiag d;
    struct i2sStats st;
    stm32i2s_getDiag(&d);
    stm32i2s_getStats(&st, false);

    serLog("STAT irq=%lu to=%lu run=%lu sr=%04lx ws=%lu ck=%lu sd=%lu/%lu "
           "frames=%lu L=%d/%u R=%d/%u raw=%d,%d,%d,%d,%d,%d,%d,%d",
           (unsigned long) d.irqs, (unsigned long) d.timeouts,
           (unsigned long) d.running, (unsigned long) d.sr,
           (unsigned long) d.wsEdges, (unsigned long) d.ckEdges,
           (unsigned long) d.sdHigh, (unsigned long) d.polls,
           (unsigned long) st.frames, st.peak[0], st.rms[0], st.peak[1],
           st.rms[1], d.raw[0], d.raw[1], d.raw[2], d.raw[3], d.raw[4],
           d.raw[5], d.raw[6], d.raw[7]);
}

/**
 * Record ms milliseconds and report.
 */
static void cmdRec(const uint32_t ms, const bool dump)
{
    struct i2sStats st;
    size_t want = (size_t) ms * (SAMPLE_RATE / 1000);

    stm32i2s_getStats(&st, true);
    setPhase(PARROT_REC, 0);
    long long t0 = getTick();
    int n = record(want);
    long long t1 = getTick();
    stm32i2s_getStats(&st, true);
    setPhase(PARROT_IDLE, (n < 0) ? n : 0);

    recLen = (n > 0) ? (size_t) n : 0;
    pthread_mutex_lock(&mutex);
    status.stats = st;
    status.takes++;
    pthread_mutex_unlock(&mutex);

    serLog("TAKE %lu n=%d %ld ms frames=%lu L=%d/%u mean %d R=%d/%u mean %d",
           (unsigned long) takeNum, n, (long) (t1 - t0),
           (unsigned long) st.frames, st.peak[0], st.rms[0], st.mean[0],
           st.peak[1], st.rms[1], st.mean[1]);

    if(dump && (recLen > 0))
        serDump(takeNum, rec, recLen);

    takeNum++;
}

/**
 * Play the last take.
 */
static void cmdPlay()
{
    if(recLen == 0)
    {
        serLog("ERR nothing recorded");
        return;
    }

    uint8_t gain = normalise(recLen);
    serLog("PLAY gain x%u", gain);
    setPhase(PARROT_PLAY, 0);
    int ret = play(recLen);
    setPhase(PARROT_IDLE, (int16_t) ret);
}

/**
 * Play a generated tone through the same path.
 */
static void cmdTone(const uint32_t hz, const uint32_t ms)
{
    size_t n = (size_t) ms * (SAMPLE_RATE / 1000);
    if(n > REC_SAMPLES)
        n = REC_SAMPLES;

    for(size_t i = 0; i < n; i++)
        rec[i] = (stream_sample_t) (8000.0f * sinf(6.2831853f * (float) hz
                                                   * (float) i / SAMPLE_RATE));
    recLen = n;
    setPhase(PARROT_PLAY, 0);
    int ret = play(recLen);
    setPhase(PARROT_IDLE, (int16_t) ret);
}

static void cmdHelp()
{
    serLog("cmds: help | stat | mon 0/1 | rec <ms> [q] | play | tone <hz> <ms> |");
    serLog("      w <reg> <val> | r <reg> [n] | rs <reg> <mode0-2> <us> [n] | ra <page> <reg> [mode] [us] |");
    serLog("      c6 init|fm|dmr|beepoff | c6 lo 0/1 | c6 beep <hz> | ch l/r |");
    serLog("      spk 1/0 (hold MCU->SPK path) | mic 1/0 (hold MIC->MCU path) | sleep <ms>");
    serLog("      rdly [n] (fast read settle NOPs) | rtest <reg> [n] | fmcap <ms> [i=invert]");
    serLog("      sym clr | sym <hex dibits> | dev <mod1> <mod2> | phytx [max slots, 200 cap]");
    serLog("numbers: hex for reg/val/page (no 0x), decimal otherwise");
}

/**
 * Run one console line.
 */
static void runLine(char *line)
{
    static pathId spkPath = -1;
    static pathId micPath = -1;
    char *argv[6];
    int argc = 0;

    for(char *tok = strtok(line, " \t"); tok && (argc < 6); tok = strtok(NULL, " \t"))
        argv[argc++] = tok;

    if(argc == 0)
        return;

    const char *c = argv[0];
    #define HEX(i) ((argc > (i)) ? (uint32_t) strtoul(argv[i], NULL, 16) : 0)
    #define DEC(i, d) ((argc > (i)) ? (uint32_t) strtoul(argv[i], NULL, 10) : (d))

    if(strcmp(c, "help") == 0)       cmdHelp();
    else if(strcmp(c, "stat") == 0)  logStat();
    else if(strcmp(c, "mon") == 0)   { monitor = (DEC(1, 1) != 0); serLog("OK mon %d", monitor); }
    else if(strcmp(c, "rec") == 0)   cmdRec(DEC(1, 3000), !((argc > 2) && (argv[2][0] == 'q')));
    else if(strcmp(c, "play") == 0)  cmdPlay();
    else if(strcmp(c, "tone") == 0)  cmdTone(DEC(1, 1000), DEC(2, 1000));
    else if(strcmp(c, "sleep") == 0) { sleepFor(0, DEC(1, 100)); serLog("OK"); }
    else if(strcmp(c, "ch") == 0)
    {
        bool r = (argc > 1) && (argv[1][0] == 'r');
        stm32i2s_setChannel(r);
        serLog("OK ch %c", r ? 'R' : 'L');
    }
    else if(strcmp(c, "w") == 0)
    {
        i2sParrot_writeC6000((uint8_t) HEX(1), (uint8_t) HEX(2));
        serLog("OK w %02lx %02lx", (unsigned long) HEX(1), (unsigned long) HEX(2));
    }
    else if(strcmp(c, "r") == 0)
    {
        uint8_t reg = (uint8_t) HEX(1);
        char out[200];
        int o = snprintf(out, sizeof(out), "R %02x:", reg);
        uint32_t cnt = DEC(2, 8);
        for(uint32_t i = 0; (i < cnt) && (o < 190); i++)
        {
            uint8_t v;
            i2sParrot_readC6000(&reg, &v, 1);
            o += snprintf(out + o, sizeof(out) - o, " %02x", v);
        }
        serLog("%s", out);
    }
    else if(strcmp(c, "rs") == 0)
    {
        uint8_t reg = (uint8_t) HEX(1);
        int mode = (int) DEC(2, 0);
        uint32_t us = DEC(3, 2);
        uint32_t cnt = DEC(4, 8);
        char out[200];
        int o = snprintf(out, sizeof(out), "RS %02x m%d %luus:", reg, mode,
                         (unsigned long) us);
        for(uint32_t i = 0; (i < cnt) && (o < 190); i++)
            o += snprintf(out + o, sizeof(out) - o, " %02x",
                          slowRead(4, reg, mode, us));
        serLog("%s", out);
    }
    else if(strcmp(c, "sym") == 0)
    {
        if(sym == NULL)
            sym = (uint8_t *) malloc(SYM_MAX);

        if((argc > 1) && (strcmp(argv[1], "clr") == 0))
            symLen = 0;
        else if((argc > 1) && (sym != NULL))
        {
            const char *h = argv[1];
            while((h[0] != '\0') && (h[1] != '\0') && (symLen < SYM_MAX))
            {
                char b[3] = { h[0], h[1], 0 };
                sym[symLen++] = (uint8_t) strtoul(b, NULL, 16);
                h += 2;
            }
        }
        serLog("OK sym %u bytes", (unsigned) symLen);
    }
    else if(strcmp(c, "dev") == 0)
    {
        if(argc > 2)
        {
            devMod1 = (uint8_t) HEX(1);
            devMod2 = (uint8_t) HEX(2);
        }
        serLog("OK dev mod1 %02x mod2 %02x", devMod1, devMod2);
    }
    else if(strcmp(c, "phytx") == 0)
        cmdPhyTx(DEC(1, 120));
    else if(strcmp(c, "rdly") == 0)
    {
        if(argc > 1)
            c6000ReadDelay = DEC(1, 16);
        serLog("OK rdly %lu", (unsigned long) c6000ReadDelay);
    }
    else if(strcmp(c, "rtest") == 0)
    {
        uint8_t reg = (uint8_t) HEX(1);
        uint32_t cnt = DEC(2, 100), bad = 0;
        uint8_t ref = slowRead(4, reg, 0, 1), got = ref, fast = 0;
        for(uint32_t i = 0; i < cnt; i++)
        {
            i2sParrot_readC6000(&reg, &fast, 1);
            if(fast != ref) { bad++; got = fast; }
        }
        serLog("RTEST %02x rdly %lu: slow %02x, %lu/%lu fast reads differ (e.g. %02x)",
               reg, (unsigned long) c6000ReadDelay, ref, (unsigned long) bad,
               (unsigned long) cnt, got);
    }
    else if(strcmp(c, "fmcap") == 0)
        cmdFmCap(DEC(1, 3000), (argc > 2) && (argv[2][0] == 'i'));
    else if(strcmp(c, "ra") == 0)
    {
        uint8_t v = slowRead((uint8_t) HEX(1), (uint8_t) HEX(2), (int) DEC(3, 1), DEC(4, 2));
        serLog("RA %02lx:%02lx = %02x", (unsigned long) HEX(1), (unsigned long) HEX(2), v);
    }
    else if(strcmp(c, "c6") == 0)
    {
        int ret = (argc > 1) ? i2sParrot_c6000Op(argv[1], (int) DEC(2, 0)) : -1;
        serLog("%s c6 %s -> %d", (ret < 0) ? "ERR" : "OK", (argc > 1) ? argv[1] : "", ret);
    }
    else if(strcmp(c, "spk") == 0)
    {
        if(DEC(1, 1) != 0)
        {
            if(spkPath < 0)
                spkPath = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_PROMPT);
            serLog("OK spk path %ld", (long) spkPath);
        }
        else
        {
            if(spkPath >= 0)
                audioPath_release(spkPath);
            spkPath = -1;
            serLog("OK spk released");
        }
    }
    else if(strcmp(c, "mic") == 0)
    {
        if(DEC(1, 1) != 0)
        {
            if(micPath < 0)
                micPath = audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX);
            serLog("OK mic path %ld", (long) micPath);
        }
        else
        {
            if(micPath >= 0)
                audioPath_release(micPath);
            micPath = -1;
            serLog("OK mic released");
        }
    }
    else
        serLog("ERR unknown '%s', try help", c);

    #undef HEX
    #undef DEC
}

/**
 * Serial console: one command per line.
 */
static void *consoleThread(void *arg)
{
    (void) arg;
    char line[320];
    size_t len = 0;

    sleepFor(2, 0);
    serLog("PARROT console ready, type help");

    while(1)
    {
        char buf[32];
        ssize_t n = vcom_readBlock(buf, sizeof(buf));
        if(n <= 0)
        {
            sleepFor(0, 10);
            continue;
        }

        for(ssize_t i = 0; i < n; i++)
        {
            char ch = buf[i];
            if((ch == '\r') || (ch == '\n'))
            {
                if(len > 0)
                {
                    line[len] = '\0';
                    serLog("> %s", line);
                    runLine(line);
                    serLog("DONE");
                }
                len = 0;
            }
            else if(len < (sizeof(line) - 1))
            {
                line[len++] = ch;
            }
        }
    }

    return NULL;
}

/**
 * Once a second: driver diagnostics to the display and the USB serial port.
 */
static void *diagThread(void *arg)
{
    (void) arg;
    uint32_t sec = 0;

    sleepFor(2, 0);

    while(1)
    {
        struct i2sDiag d;
        struct i2sStats st;
        stm32i2s_getDiag(&d);
        stm32i2s_getStats(&st, false);

        pthread_mutex_lock(&mutex);
        status.diag = d;
        status.live = st;
        const uint8_t phase = status.phase;
        const uint32_t takes = status.takes;
        pthread_mutex_unlock(&mutex);

        if(monitor)
        serLog("I2S t=%lu ph=%u takes=%lu irq=%lu to=%lu pos=%lu run=%lu "
               "sr=%04lx ndtr=%lu ws=%lu ck=%lu sd=%lu/%lu frames=%lu "
               "L=%d/%u R=%d/%u noclk=%lu",
               (unsigned long) sec, phase, (unsigned long) takes,
               (unsigned long) d.irqs, (unsigned long) d.timeouts,
               (unsigned long) d.pos, (unsigned long) d.running,
               (unsigned long) d.sr, (unsigned long) d.ndtr,
               (unsigned long) d.wsEdges, (unsigned long) d.ckEdges,
               (unsigned long) d.sdHigh, (unsigned long) d.polls,
               (unsigned long) st.frames, st.peak[0], st.rms[0], st.peak[1],
               st.rms[1], (unsigned long) st.noClock);

        sec++;
        sleepFor(1, 0);
    }

    return NULL;
}

void i2sParrot_start()
{
    pthread_attr_t attr;
    pthread_t thread;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4096);
    pthread_create(&thread, &attr, consoleThread, NULL);
    pthread_create(&thread, &attr, diagThread, NULL);
}

void i2sParrot_getStatus(struct parrotStatus *out)
{
    pthread_mutex_lock(&mutex);
    *out = status;
    pthread_mutex_unlock(&mutex);
}
