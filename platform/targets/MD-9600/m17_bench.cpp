/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Bench test: M17 on the HR_C6000 direct modes, driven from the USB console.
 *
 * m17tx: microphone PCM over I2S3 -> Codec2 (audio_codec) -> M17 frames
 * (FrameEncoder) -> HR_C6000 physical layer TX RAM. The frame bytes are the
 * chip's dibits as they are: both use 00 = +1, 01 = +3, 10 = -1, 11 = -3.
 *
 * m17rx: HR_C6000 FM receive copy (8 kHz) -> x3 interpolation -> M17
 * Demodulator at 24 kHz -> FrameDecoder -> Codec2 into a recording.
 */

#include <cmath>
#include <cstring>
#include <errno.h>
#include <new>
#include <miosix.h>
#include "core/audio_codec.h"
#include "core/audio_path.h"
#include "interfaces/delays.h"
#include "protocols/M17/Demodulator.hpp"
#include "protocols/M17/FrameDecoder.hpp"
#include "protocols/M17/FrameEncoder.hpp"
#include "protocols/M17/LinkSetupFrame.hpp"
#include "codec2.h"
#include "i2s_parrot.h"

using namespace M17;

extern "C" {

typedef int (*bench_chunk_fn)(uint8_t *chunk, void *ctx);

struct benchTxStats
{
    uint32_t chunks, slots, jumps, underruns;
    bool     stall;
    long     paAfterMs, onMs;
};

int      bench_phyRun(bench_chunk_fn source, void *ctx, uint32_t maxSlots,
                      const uint8_t mod1, const uint8_t mod2,
                      struct benchTxStats *st);
void     bench_log(const char *fmt, ...);
uint8_t  bench_readReg(const uint8_t reg);
void     bench_writeReg(const uint8_t reg, const uint8_t val);
void     bench_ramRead(const uint8_t page, const uint16_t addr, uint8_t *buf,
                       const size_t n);
int16_t *bench_rec(size_t *cap);
void     bench_recDone(const size_t n, const bool dump, const bool playIt);
void     bench_setPhase(const uint8_t phase);
void     bench_devFor(const bool vhf, uint8_t *mod1, uint8_t *mod2);
bool     radio_testTxVhf();
bool     radio_testRxVhf();

void m17bench_tx(const uint32_t seconds, const char *callsign);
void m17bench_mem(const char *when);
void m17bench_rx(const uint32_t seconds, const uint32_t shift, const int invert);
}

void m17bench_mem(const char *when)
{
    bench_log("MEM %s: free heap %u of %u, lowest %u", when,
              miosix::MemoryProfiling::getCurrentFreeHeap(),
              miosix::MemoryProfiling::getHeapSize(),
              miosix::MemoryProfiling::getAbsoluteFreeHeap());
}

// --- m17tx -----------------------------------------------------------------

static constexpr size_t SLOT_BYTES = 36;
static constexpr size_t FIFO_SIZE  = 1024;

struct TxCtx
{
    FrameEncoder encoder;
    uint8_t      fifo[FIFO_SIZE];
    size_t       head = 0, count = 0;
    payload_t    payload;
    size_t       half = 0;          // Codec2 frames in payload (0 or 1)
    uint32_t     frames = 0, wantFrames = 0;
    bool         done = false;
    uint32_t     codecPops = 0;

    void push(const uint8_t *b, size_t n)
    {
        for(size_t i = 0; (i < n) && (count < FIFO_SIZE); i++)
        {
            fifo[(head + count) % FIFO_SIZE] = b[i];
            count++;
        }
    }

    size_t pop(uint8_t *b, size_t n)
    {
        size_t got = (n < count) ? n : count;
        for(size_t i = 0; i < got; i++)
            b[i] = fifo[(head + i) % FIFO_SIZE];
        head   = (head + got) % FIFO_SIZE;
        count -= got;
        return got;
    }

    // Encode stream frames while the codec has data, keeping a few slots queued
    void topUp()
    {
        while((done == false) && (count < 4 * SLOT_BYTES))
        {
            if(codec_popFrame(payload.data() + 8 * half, false) < 0)
                return;

            codecPops++;
            if(++half < 2)
                continue;

            half = 0;
            frame_t frame;
            frames++;
            const bool last = (frames >= wantFrames);
            encoder.encodeStreamFrame(payload, frame, last);
            push(frame.data(), frame.size());

            if(last)
            {
                encoder.encodeEotFrame(frame);
                push(frame.data(), frame.size());
                done = true;
            }
        }
    }
};

static int txChunk(uint8_t *chunk, void *ctx)
{
    TxCtx *t = static_cast< TxCtx * >(ctx);
    t->topUp();

    if(t->count >= SLOT_BYTES)
        return (int) t->pop(chunk, SLOT_BYTES);

    if(t->done)
        return (t->count > 0) ? (int) t->pop(chunk, t->count) : 0;

    return -EAGAIN;
}

void m17bench_tx(uint32_t seconds, const char *callsign)
{
    if(seconds > 10)
        seconds = 10;

    TxCtx *t = new TxCtx();
    t->wantFrames = seconds * 25;   // 40 ms stream frames

    LinkSetupFrame lsf;
    lsf.clear();
    lsf.setSource(Callsign(callsign));
    streamType_t type;
    type.value          = 0;
    type.fields.dataMode = DATAMODE_STREAM;
    type.fields.dataType = DATATYPE_VOICE;
    lsf.setType(type);

    frame_t frame;
    t->encoder.reset();
    t->encoder.encodeLsf(lsf, frame);

    // Preamble (+3 -3, one frame) and the link setup frame
    uint8_t pre[48];
    memset(pre, 0x77, sizeof(pre));
    t->push(pre, sizeof(pre));
    t->push(frame.data(), frame.size());

    pathId path = audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX);
    if((path < 0) || (codec_startEncode(path) == false))
    {
        bench_log("ERR m17tx: mic path %ld / codec start", (long) path);
        if(path >= 0)
            audioPath_release(path);
        delete t;
        return;
    }

    // One stream frame queued before keying, as margin for the slot clock
    long long w0 = getTick();
    while((t->frames < 1) && (getTick() - w0 < 500))
    {
        t->topUp();
        sleepFor(0, 5);
    }

    uint8_t mod1, mod2;
    const bool vhf = radio_testTxVhf();
    bench_devFor(vhf, &mod1, &mod2);

    benchTxStats st;
    int ret = bench_phyRun(txChunk, t, seconds * 34 + 40, mod1, mod2, &st);

    codec_stop(path);
    audioPath_release(path);

    if(ret < 0)
        bench_log("ERR m17tx prep %d (not V5/no band = -1, supply = -2)", ret);
    else
        bench_log("M17TX %s %s: %lu stream frames + EOT, %lu codec frames, %lu "
                  "chunks, %lu slots, %lu underruns, %lu jumps%s, on %ld ms, "
                  "dev %02x/%02x", callsign, vhf ? "VHF" : "UHF",
                  (unsigned long) t->frames, (unsigned long) t->codecPops,
                  (unsigned long) st.chunks, (unsigned long) st.slots,
                  (unsigned long) st.underruns, (unsigned long) st.jumps,
                  st.stall ? " STALLED" : "", st.onMs, mod1, mod2);
    delete t;
}

// --- m17rx -----------------------------------------------------------------

static constexpr uint16_t COPY_RAM  = 0x030;
static constexpr size_t   COPY_HALF = 512;       // bytes, 256 samples
static constexpr size_t   PHASES    = 3;         // 8 kHz -> 24 kHz
static constexpr size_t   TAPS      = 12;        // per phase

/**
 * Polyphase x3 interpolator: windowed sinc low-pass at 3.6 kHz (24 kHz rate),
 * gain 3 to keep the level.
 */
struct Interp
{
    float h[PHASES][TAPS];
    float x[TAPS] = {};

    Interp()
    {
        const size_t N = PHASES * TAPS;
        const float fc = 3600.0f / 24000.0f;
        for(size_t n = 0; n < N; n++)
        {
            float m = (float) n - (float) (N - 1) / 2.0f;
            float s = (m == 0.0f) ? 2.0f * fc
                                  : sinf(2.0f * (float) M_PI * fc * m) / ((float) M_PI * m);
            float w = 0.54f - 0.46f * cosf(2.0f * (float) M_PI * (float) n / (float) (N - 1));
            h[n % PHASES][n / PHASES] = 3.0f * s * w;
        }
    }

    void push(float v)
    {
        memmove(&x[1], &x[0], (TAPS - 1) * sizeof(float));
        x[0] = v;
    }

    float out(size_t p) const
    {
        float acc = 0.0f;
        for(size_t k = 0; k < TAPS; k++)
            acc += h[p][k] * x[k];
        return acc;
    }
};

static int16_t copyLast(const int half)
{
    uint8_t b[2];
    bench_ramRead(2, (uint16_t) (COPY_RAM + half * COPY_HALF + COPY_HALF - 2), b, 2);
    return (int16_t) ((b[0] << 8) | b[1]);
}

void m17bench_rx(uint32_t seconds, const uint32_t shift, const int invertArg)
{
    static const uint8_t saveRegs[] = { 0x41, 0x36, 0x60, 0x34, 0x10, 0x81 };
    uint8_t saved[sizeof(saveRegs)];
    uint8_t half[COPY_HALF];

    if(seconds > 60)
        seconds = 60;

    // The VHF receive I/Q order inverts the copy (RadioFoundry 10.6)
    const bool invert = (invertArg < 0) ? radio_testRxVhf() : (invertArg != 0);

    m17bench_mem("m17rx start");
    bench_log("sizes: Demodulator %u, FrameDecoder %u, Interp %u",
              (unsigned) sizeof(Demodulator), (unsigned) sizeof(FrameDecoder),
              (unsigned) sizeof(Interp));

    Demodulator  *demod   = new (std::nothrow) Demodulator();
    m17bench_mem("after Demodulator");
    FrameDecoder *decoder = new (std::nothrow) FrameDecoder();
    m17bench_mem("after FrameDecoder");
    Interp       *interp  = new (std::nothrow) Interp();
    m17bench_mem("after Interp");
    struct CODEC2 *c2     = ((demod != nullptr) && (decoder != nullptr) && (interp != nullptr))
                          ? codec2_create(CODEC2_MODE_3200) : nullptr;
    m17bench_mem("after codec2_create");

    if((demod == nullptr) || (decoder == nullptr) || (interp == nullptr) || (c2 == nullptr))
    {
        bench_log("ERR m17rx: out of memory (demod %p decoder %p interp %p codec2 %p)",
                  demod, decoder, interp, c2);
        if(c2 != nullptr) codec2_destroy(c2);
        delete interp;
        delete decoder;
        delete demod;
        return;
    }

    demod->init();
    m17bench_mem("after Demodulator::init");
    decoder->reset();

    size_t cap;
    int16_t *rec = bench_rec(&cap);
    size_t recN = 0;

    for(size_t i = 0; i < sizeof(saveRegs); i++)
        saved[i] = bench_readReg(saveRegs[i]);

    bench_writeReg(0x41, 0x00);
    bench_writeReg(0x36, 0x00);
    bench_writeReg(0x60, 0x00);
    bench_writeReg(0x34, 0x00);
    bench_writeReg(0x10, 0x80);
    bench_writeReg(0x81, (uint8_t) (saved[5] | 0x02));
    bench_writeReg(0x83, 0xFF);

    int16_t last[2] = { copyLast(0), copyLast(1) };
    int lastHalf = 1;
    uint32_t halves = 0, newFrames = 0, lsfFrames = 0, streamFrames = 0;
    uint32_t eots = 0, locks = 0, other = 0;
    bool locked = false;
    char src[12] = "", dst[12] = "";

    bench_setPhase(PARROT_REC);
    long long t0 = getTick();
    const long long tEnd = t0 + (long long) seconds * 1000;

    while(getTick() < tEnd)
    {
        if((bench_readReg(0x82) & 0x02) == 0)
        {
            sleepFor(0, 2);
            continue;
        }

        bench_writeReg(0x83, 0x02);
        const int16_t now[2] = { copyLast(0), copyLast(1) };
        const bool ch0 = (now[0] != last[0]);
        const bool ch1 = (now[1] != last[1]);
        int h = (ch0 && !ch1) ? 0 : ((ch1 && !ch0) ? 1 : (lastHalf ^ 1));

        bench_ramRead(2, (uint16_t) (COPY_RAM + h * COPY_HALF), half, COPY_HALF);
        last[0]  = now[0];
        last[1]  = now[1];
        lastHalf = h;
        halves++;

        for(size_t i = 0; i < COPY_HALF / 2; i++)
        {
            int16_t v = (int16_t) ((half[2 * i] << 8) | half[2 * i + 1]);
            interp->push((float) (v >> shift));

            for(size_t p = 0; p < PHASES; p++)
            {
                float y = interp->out(p);
                if(y > 32767.0f)  y = 32767.0f;
                if(y < -32768.0f) y = -32768.0f;

                bool ready = demod->sample((int16_t) y, invert);
                bool lock  = demod->isLocked();
                if(lock && !locked)
                {
                    decoder->reset();
                    locks++;
                }
                locked = lock;

                if(!(ready && locked))
                    continue;

                newFrames++;
                auto type = decoder->decodeFrame(demod->getFrame());
                const LinkSetupFrame &lsf = decoder->getLsf();
                if(lsf.valid() && (src[0] == '\0'))
                {
                    LinkSetupFrame l = lsf;
                    strncpy(src, (const char *) l.getSource(), sizeof(src) - 1);
                    strncpy(dst, (const char *) l.getDestination(), sizeof(dst) - 1);
                }

                if(type == FrameType::LINK_SETUP)
                    lsfFrames++;
                else if(type == FrameType::EOT)
                    eots++;
                else if(type == FrameType::STREAM)
                {
                    streamFrames++;
                    const StreamFrame &sf = decoder->getStreamFrame();
                    for(size_t k = 0; (k < 2) && (recN + 160 <= cap); k++)
                    {
                        codec2_decode(c2, &rec[recN], sf.data() + 8 * k);
                        recN += 160;
                    }
                }
                else
                    other++;
            }
        }
    }

    for(size_t i = 0; i < sizeof(saveRegs); i++)
    {
        const uint8_t r = saveRegs[sizeof(saveRegs) - 1 - i];
        bench_writeReg(r, saved[sizeof(saveRegs) - 1 - i]);
    }
    bench_writeReg(0x83, 0xFF);
    bench_setPhase(PARROT_IDLE);

    bench_log("M17RX %lu s %s%s shift %lu: %lu halves, %lu locks, %lu frames "
              "(%lu LSF, %lu stream, %lu EOT, %lu other), src '%s' dst '%s', "
              "%u ms of audio", (unsigned long) seconds,
              radio_testRxVhf() ? "VHF" : "UHF", invert ? " inverted" : "",
              (unsigned long) shift, (unsigned long) halves,
              (unsigned long) locks, (unsigned long) newFrames,
              (unsigned long) lsfFrames, (unsigned long) streamFrames,
              (unsigned long) eots, (unsigned long) other, src, dst,
              (unsigned) (recN / 8));

    codec2_destroy(c2);
    demod->terminate();
    delete interp;
    delete decoder;
    delete demod;

    bench_recDone(recN, true, true);
}
