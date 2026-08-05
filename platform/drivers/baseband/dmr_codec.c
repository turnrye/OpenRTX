/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * DMR AMBE+2 vocoder driver. The vocoder is a proprietary blob installed at a
 * fixed flash address from the user's own donor firmware (see the separate DMR
 * installer); it is never part of the OpenRTX image. This file contains no
 * codec bytes -- only the addresses of the donor's own functions and the fixed
 * parameters to drive them, exactly as the donor firmware does. The blob uses
 * only the state pointers we pass it (verified: it holds no absolute references
 * to its buffers), so the state lives in our RAM.
 *
 * The entry points and the initialisation sequence were recovered from the
 * donor by static analysis; they are facts about the interface, like the
 * HR-C6000 register map, not copied code.
 */

#include "dmr_codec.h"
#include "core/dmr_debug.h"
#include <string.h>

/*
 * The vocoder blob. Its first instruction is push {r4-r11,lr}, encoded as
 * 0x4FF0E92D; that word doubles as the "codec installed" magic.
 */
#define AMBE_BLOB_BASE      0x0807537Cu
#define AMBE_BLOB_MAGIC     0x4FF0E92Du

/* Thumb entry points inside the blob (bit 0 set marks a Thumb target). */
#define THUMB(a)            ((a) | 1u)
#define AMBE_INIT_ENC       THUMB(0x08075600u)  /* init encoder state (buf,1,1)   */
#define AMBE_INIT_DEC       THUMB(0x08075A4Cu)  /* init decoder state (buf,1)      */
#define AMBE_CFG_ENC        THUMB(0x08075BC4u)  /* encoder config (buf, 10 params) */
#define AMBE_CFG_A          THUMB(0x0807553Cu)  /* (buf, 0x80, -20)                */
#define AMBE_CFG_B          THUMB(0x08075A04u)  /* (buf, 0x80, -20)                */
#define AMBE_ENCODE         THUMB(0x080754ACu)  /* one 80-sample encode half       */
#define AMBE_ENCODE_ECC     THUMB(0x08075864u)  /* add FEC to the 72-bit frame     */
#define AMBE_DECODE         THUMB(0x08075954u)  /* one 80-sample decode half       */

/* State buffer sizes, from the donor init routines' clear/write extents. */
#define ENC_STATE_SIZE      0x2000
#define DEC_STATE_SIZE      0x07EC
#define ECC_STATE_SIZE      0x0100

/* Half-frame selector values passed to the encoder for the two 80-sample halves */
#define ENC_MODE_HALF0      0x1840
#define ENC_MODE_HALF1      0x0800

/* Function-pointer views of the blob entries. */
typedef void (*ambe_init_enc_fn)(void *st, int a, int b);
typedef void (*ambe_init_dec_fn)(void *st, int a);
typedef void (*ambe_cfg_enc_fn)(void *st, int, int, int, int, int, int, int, int,
                                int, int);
typedef void (*ambe_cfg_fn)(void *st, int a, int b);
typedef void (*ambe_encode_fn)(void *bits, int, void *wave, int count, int mode,
                               int half, int, void *st);
typedef void (*ambe_ecc_fn)(void *bits, void *bits2, int zero, void *ecc);
typedef void (*ambe_decode_fn)(void *wave, int count, void *bits, int, int,
                               int half, void *st);

static uint8_t encState[ENC_STATE_SIZE] __attribute__((aligned(4)));
static uint8_t decState[DEC_STATE_SIZE] __attribute__((aligned(4)));
static uint8_t eccState[ECC_STATE_SIZE] __attribute__((aligned(4)));
static bool    ready = false;

bool dmr_codec_available(void)
{
    return *(const volatile uint32_t *)AMBE_BLOB_BASE == AMBE_BLOB_MAGIC;
}

bool dmr_codec_init(void)
{
    if(dmr_codec_available() == false)
    {
        ready = false;
        return false;
    }

    memset(encState, 0, sizeof(encState));
    memset(decState, 0, sizeof(decState));
    memset(eccState, 0, sizeof(eccState));

    /*
     * The donor firmware's setup runs this exact sequence before the first
     * encode/decode: bulk-init the encoder and decoder state, then apply the
     * fixed configuration. The parameters are the constants the donor uses;
     * they are rate/scale settings, not codec data.
     */
    ((ambe_init_enc_fn) AMBE_INIT_ENC)(encState, 1, 1);
    ((ambe_init_dec_fn) AMBE_INIT_DEC)(decState, 1);
    ((ambe_cfg_enc_fn)  AMBE_CFG_ENC )(encState, -22, 20, -10, -40, 0x4000,
                                       0xCCC, 0xCCC, 0x51E, 10, 20);
    ((ambe_cfg_fn)      AMBE_CFG_A   )(encState, 0x80, -20);
    ((ambe_cfg_fn)      AMBE_CFG_B   )(decState, 0x80, -20);

    ready = true;
    return true;
}

void dmr_codec_terminate(void)
{
    ready = false;
}

void dmr_codec_encode(uint8_t frame[DMR_CODEC_FRAME_BYTES],
                      const int16_t pcm[DMR_CODEC_PCM_SAMPLES])
{
    uint8_t bits[72];

    memset(frame, 0, DMR_CODEC_FRAME_BYTES);
    if(ready == false)
        return;

    memset(bits, 0, sizeof(bits));

    /*
     * A 20 ms frame is encoded in two 80-sample halves, then protected. The
     * encoder keeps the pitch history in its state across the two calls; the
     * mode word and the half flag distinguish them. NOTE: the exact wave
     * pointer handling for the two halves is confirmed on-radio by the
     * golden-vector loopback before any voice test.
     */
    ((ambe_encode_fn) AMBE_ENCODE)(bits, 0, (void *) pcm, 80, ENC_MODE_HALF0, 0,
                                   0x2000, encState);
    ((ambe_encode_fn) AMBE_ENCODE)(bits, 0, (void *) (pcm + 80), 80,
                                   ENC_MODE_HALF1, 1, 0x2000, encState);
    ((ambe_ecc_fn) AMBE_ENCODE_ECC)(bits, bits, 0, eccState);

    /* Pack the 72 protected bits MSB first into the 9-byte AMBE frame. */
    for(int i = 0; i < 72; i++)
    {
        if(bits[i] & 1)
            frame[i >> 3] |= 0x80 >> (i & 7);
    }
}

/*
 * The DMR AMBE de-interleave. The 72 on-air bits of a frame are scattered into
 * the four codewords (a flat [4][24] buffer) by these fixed position tables,
 * then the second codeword is de-scrambled and the frame is repacked into the
 * 49 parameter bits the vocoder decodes. This is the standard DMR AMBE frame
 * layout; the decoder cannot use the raw frame directly.
 */
static const uint8_t ambeDeintA[36] =
{
    23, 34, 22, 33, 21, 32, 20, 31, 19, 30, 18, 29, 17, 28, 16, 27, 15, 26,
    14, 25, 13, 24, 12, 58, 11, 57, 10, 56,  9, 55,  8, 54,  7, 53,  6, 52
};
static const uint8_t ambeDeintB[36] =
{
     5, 51,  4, 50,  3, 49,  2, 48,  1, 85,  0, 84, 45, 83, 45, 82, 44, 81,
    43, 80, 42, 79, 41, 78, 40, 77, 39, 76, 38, 75, 37, 74, 36, 73, 35, 72
};

/**
 * Turn a 9-byte on-air AMBE frame into the 49-bit parameter buffer the vocoder
 * decodes. De-interleave into four codewords, de-scramble the second one with a
 * PRNG seeded from the first, then repack. Error correction on the two protected
 * codewords is not applied here; a clean frame needs none.
 */
static void ambeInitFrame(const uint8_t *frame, uint16_t bits[49])
{
    uint8_t cw[96];
    memset(cw, 0, sizeof(cw));

    int pos = 0;
    for(int i = 0; i < 9; i++)
    {
        for(int j = 7; j > 0; j -= 2)
        {
            cw[ambeDeintA[pos]] = (frame[i] >> j)       & 1;
            cw[ambeDeintB[pos]] = (frame[i] >> (j - 1)) & 1;
            pos++;
        }
    }

    uint8_t *c0 = cw;         /* codeword 0 */
    uint8_t *c1 = cw + 24;    /* codeword 1, scrambled */
    uint8_t *c2 = cw + 48;    /* codeword 2 */
    uint8_t *c3 = cw + 72;    /* codeword 3 */

    /* De-scramble c1 with a PRNG seeded from c0's high bits. */
    uint32_t seed = 0;
    for(int i = 23; i >= 12; i--)
        seed = (seed << 1) | c0[i];

    uint16_t prng[24];
    uint32_t p = (seed << 4) & 0xFFFF;
    prng[0] = (uint16_t) p;
    for(int i = 0; i < 23; i++)
    {
        p = (0xAD * p + 0x3619) & 0xFFFF;
        prng[i + 1] = (uint16_t) p;
    }
    for(int i = 0; i < 23; i++)
        c1[i] ^= (prng[23 - i] >> 15) & 1;

    /* Repack into the 49 parameter bits (c3, c2, c1, c0 order). */
    int o = 48;
    for(int i = 0;  i < 14; i++) bits[o--] = c3[i];
    for(int i = 0;  i < 11; i++) bits[o--] = c2[i];
    for(int i = 11; i < 23; i++) bits[o--] = c1[i];
    for(int i = 12; i < 24; i++) bits[o--] = c0[i];
}

/*
 * Golay(23,12) syndrome of a 23-bit codeword, generator 0xC75. Zero for a valid
 * codeword.
 */
static uint32_t golaySyndrome(uint32_t cw)
{
    for(int i = 22; i >= 11; i--)
    {
        if(cw & (1u << i))
            cw ^= (uint32_t) 0xC75 << (i - 11);
    }
    return cw & 0x7FF;
}

static int popcount11(uint32_t x)
{
    int n = 0;
    while(x)
    {
        n += x & 1;
        x >>= 1;
    }
    return n;
}

/**
 * Estimate the number of bit errors in a 23-bit Golay codeword (capped at 4).
 * Used only to tell a real voice burst from noise on the idle timeslot.
 *
 * @param bits: 23 codeword bits, most significant first.
 */
static int golayErrors(const uint8_t *bits)
{
    uint32_t cw = 0;
    for(int i = 0; i < 23; i++)
        cw = (cw << 1) | bits[i];

    int s = popcount11(golaySyndrome(cw));
    if(s <= 3)
        return s;

    for(int i = 0; i < 23; i++)
    {
        int s2 = popcount11(golaySyndrome(cw ^ (1u << i)));
        if(s2 <= 2)
            return 1 + s2;
    }
    return 4;
}

bool dmr_codec_frameIsVoice(const uint8_t frame[DMR_CODEC_FRAME_BYTES])
{
    /*
     * An all-ones frame is the chip's idle payload; its codeword happens to be a
     * valid Golay word, so reject it before the error-count check below.
     */
    int ones = 0;
    for(int i = 0; i < DMR_CODEC_FRAME_BYTES; i++)
    {
        if(frame[i] == 0xFF)
            ones++;
    }
    if(ones >= DMR_CODEC_FRAME_BYTES - 1)
        return false;

    uint8_t cw[96];
    memset(cw, 0, sizeof(cw));

    int pos = 0;
    for(int i = 0; i < 9; i++)
    {
        for(int j = 7; j > 0; j -= 2)
        {
            cw[ambeDeintA[pos]] = (frame[i] >> j)       & 1;
            cw[ambeDeintB[pos]] = (frame[i] >> (j - 1)) & 1;
            pos++;
        }
    }

    /* Codeword 0 is Golay-protected; on the voice slot it decodes almost error
     * free, on the empty slot it is noise. Keep frames with at most one error. */
    uint8_t c0[23];
    for(int i = 0; i < 23; i++)
        c0[i] = cw[23 - i];        /* codeword-0 bits 23..1, MSB first */

    return golayErrors(c0) <= 1;
}

void dmr_codec_decode(int16_t pcm[DMR_CODEC_PCM_SAMPLES],
                      const uint8_t frame[DMR_CODEC_FRAME_BYTES])
{
    uint16_t bits[49];

    memset(pcm, 0, DMR_CODEC_PCM_SAMPLES * sizeof(int16_t));
    if(ready == false)
        return;

    ambeInitFrame(frame, bits);

    /* Two 80-sample halves per frame, decoder state carried across the pair. */
    ((ambe_decode_fn) AMBE_DECODE)(pcm, 0x50, bits, 0, 0, 0, decState);
    ((ambe_decode_fn) AMBE_DECODE)(pcm + 80, 0x50, bits, 0, 0, 1, decState);
}

/*
 * A single 9-byte AMBE silence frame, the standard public pattern. Decoding it
 * must return near silence, which is a safe end-to-end check of the decode path
 * that needs no captured audio.
 */
static const uint8_t selfTestSilence[DMR_CODEC_FRAME_BYTES] =
{
    0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B
};

/* One cycle of a 1 kHz tone sampled at 8 kHz, the encode test input. */
static const int16_t selfTestTone[8] =
{
    0, 5657, 8000, 5657, 0, -5657, -8000, -5657
};

#define SELFTEST_FRAMES     4

static int16_t selfTestPcm[DMR_CODEC_PCM_SAMPLES];
static int16_t selfTestOut[DMR_CODEC_PCM_SAMPLES];

/**
 * Return the peak absolute sample of the decoded PCM, a cheap health metric.
 */
static int16_t pcmPeak(const int16_t *pcm)
{
    int16_t peak = 0;
    for(int i = 0; i < DMR_CODEC_PCM_SAMPLES; i++)
    {
        int16_t a = (pcm[i] < 0) ? (int16_t) -pcm[i] : pcm[i];
        if(a > peak)
            peak = a;
    }
    return peak;
}

void dmr_codec_selfTest(void)
{
    if(dmr_codec_available() == false)
    {
        dmrDebug_log(DMR_DBG_INFO, "codec: not installed (no donor blob at "
                     "0x%08lX)", (unsigned long) AMBE_BLOB_BASE);
        return;
    }

    dmrDebug_log(DMR_DBG_INFO, "codec: blob present, self-test start");
    if(dmr_codec_init() == false)
    {
        dmrDebug_log(DMR_DBG_INFO, "codec: init failed");
        return;
    }
    dmrDebug_log(DMR_DBG_INFO, "codec: init ok");

    /* Decode path: decoding the silence frame must come back as near silence. */
    dmr_codec_decode(selfTestPcm, selfTestSilence);
    dmrDebug_log(DMR_DBG_INFO, "codec: decode silence peak %d (expect ~0)",
                 pcmPeak(selfTestPcm));

    /*
     * Encode path: encode a few frames of the tone so the encoder's inter-frame
     * state settles, streaming each 9-byte AMBE frame as a "$CE" record. An
     * off-radio decoder (mbelib) turns these back into audio to confirm the
     * encode is producing valid AMBE.
     */
    uint8_t frame[DMR_CODEC_FRAME_BYTES];
    for(int i = 0; i < DMR_CODEC_PCM_SAMPLES; i++)
        selfTestPcm[i] = selfTestTone[i & 7];

    for(int f = 0; f < SELFTEST_FRAMES; f++)
    {
        dmr_codec_encode(frame, selfTestPcm);
        dmrDebug_record(DMR_DBG_INFO, "CE", frame, sizeof(frame));
    }
    dmrDebug_log(DMR_DBG_INFO, "codec: encoded %d tone frames", SELFTEST_FRAMES);

    /* Round-trip one frame through encode then decode; a tone in should give a
     * loud, tone-like output. */
    dmr_codec_encode(frame, selfTestPcm);
    dmr_codec_decode(selfTestOut, frame);
    dmrDebug_log(DMR_DBG_INFO, "codec: self-test done, round-trip peak %d",
                 pcmPeak(selfTestOut));
}
