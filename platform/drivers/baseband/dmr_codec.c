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

void dmr_codec_decode(int16_t pcm[DMR_CODEC_PCM_SAMPLES],
                      const uint8_t frame[DMR_CODEC_FRAME_BYTES])
{
    uint8_t bits[72];

    memset(pcm, 0, DMR_CODEC_PCM_SAMPLES * sizeof(int16_t));
    if(ready == false)
        return;

    /* Unpack the 9-byte AMBE frame into 72 bits, MSB first. */
    for(int i = 0; i < 72; i++)
        bits[i] = (frame[i >> 3] >> (7 - (i & 7))) & 1;

    /*
     * Two 80-sample halves per frame, decoder state carried across. NOTE: the
     * DMR bit ordering the decoder expects (the donor applies a Golay-protected
     * de-interleave to the on-air frame first) is confirmed on-radio by the
     * golden-vector loopback before any voice test; this straight unpack is the
     * starting point for that bring-up.
     */
    ((ambe_decode_fn) AMBE_DECODE)(pcm, 0x50, bits, 0, 8, 0, decState);
    ((ambe_decode_fn) AMBE_DECODE)(pcm + 80, 0x50, bits, 0, 8, 1, decState);
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

static int16_t selfTestPcm[DMR_CODEC_PCM_SAMPLES];

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

    /*
     * Decode path: decoding the silence frame must come back as near silence.
     * Streaming the frame back as a "$CE" record lets an off-radio decoder
     * cross-check. The encode path is validated separately: it is not exercised
     * here yet, as its bring-up is still in progress.
     */
    dmr_codec_decode(selfTestPcm, selfTestSilence);
    dmrDebug_record(DMR_DBG_INFO, "CE", selfTestSilence, DMR_CODEC_FRAME_BYTES);
    dmrDebug_log(DMR_DBG_INFO, "codec: decode silence peak %d (expect ~0), done",
                 pcmPeak(selfTestPcm));
}
