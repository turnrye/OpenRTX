/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * DMR AMBE+2 vocoder driver. The vocoder is proprietary code that is never part
 * of the OpenRTX image, run from where it already sits in flash:
 *
 * - on the MD-UV3x0, the MD9600 donor firmware's codec, which the separate DMR
 *   installer merges into the image at the address the donor runs it from;
 * - on the CS7000 Plus, the copy in the radio's own stock firmware. A factory
 *   unit keeps the stock firmware in flash bank 1 while OpenRTX runs from bank
 *   2, so the codec is left where it is.
 *
 * Both are the same codec with the same calling convention, at different
 * addresses. This file contains no codec bytes: only the addresses of the
 * codec's functions and the fixed parameters to drive them. The codec uses only
 * the state pointers we pass it, so its state lives in our RAM.
 *
 * The entry points and the initialisation sequence were recovered by static
 * analysis; they are facts about the interface, like the HR-C6000 register map,
 * not copied code.
 */

#include "interfaces/dmr_codec.h"
#include "core/dmr_debug.h"
#include <pthread.h>
#include <string.h>

/* Thumb entry points inside the codec (bit 0 set marks a Thumb target). */
#define THUMB(a) ((a) | 1u)

#if defined(PLATFORM_CS7000P)

/*
 * The codec of the CS7000 Plus stock firmware V9.00.96. It lies in flash bank 1
 * sectors 5 to 7, along with the firmware's version string, and only that exact
 * build is run: the CRC32 of the region must match before any of it is called.
 */
#define AMBE_REGION_START 0x080A0000u
#define AMBE_REGION_END 0x080F6A56u
#define AMBE_REGION_CRC32 0x49205C0Eu

#define AMBE_INIT_ENC THUMB(0x080A51B8u)
#define AMBE_INIT_DEC THUMB(0x080A5614u)
#define AMBE_CFG_ENC THUMB(0x080A5778u)
#define AMBE_CFG_A THUMB(0x080A5104u)
#define AMBE_CFG_B THUMB(0x080A55D0u)
#define AMBE_ENCODE THUMB(0x080A5070u)
#define AMBE_ENCODE_ECC THUMB(0x080A5438u)
#define AMBE_DECODE THUMB(0x080A5528u)
#define AMBE_ECC_INIT THUMB(0x080A5420u)
#define AMBE_FEC_DEC THUMB(0x080A5A04u)
#define AMBE_FEC_STATS THUMB(0x080A5F20u)
#define AMBE_DEC_SETSTATS THUMB(0x080A54ECu)

#else

/*
 * The codec of the MD9600 donor firmware V26.45. Its first instruction is
 * push {r4-r11,lr}, encoded as 0x4FF0E92D; that word doubles as the "codec
 * installed" magic.
 */
#define AMBE_REGION_START 0x0807537Cu
#define AMBE_BLOB_MAGIC 0x4FF0E92Du

#define AMBE_INIT_ENC THUMB(0x08075600u)
#define AMBE_INIT_DEC THUMB(0x08075A4Cu)
#define AMBE_CFG_ENC THUMB(0x08075BC4u)
#define AMBE_CFG_A THUMB(0x0807553Cu)
#define AMBE_CFG_B THUMB(0x08075A04u)
#define AMBE_ENCODE THUMB(0x080754ACu)
#define AMBE_ENCODE_ECC THUMB(0x08075864u)
#define AMBE_DECODE THUMB(0x08075954u)
#define AMBE_ECC_INIT THUMB(0x08075848u)
#define AMBE_FEC_DEC THUMB(0x08075EE0u)
#define AMBE_FEC_STATS THUMB(0x080763DCu)
#define AMBE_DEC_SETSTATS THUMB(0x0807591Cu)

#endif

/*
 * What each entry point does:
 *
 * INIT_ENC, INIT_DEC  initialise the encoder state (buf, 1, 1) and the decoder
 *                     state (buf, 1)
 * CFG_ENC             encoder configuration (buf, 10 parameters)
 * CFG_A, CFG_B        encoder and decoder settings (buf, 0x80, -20)
 * ENCODE, DECODE      encode or decode one 80-sample half of a frame
 * ECC_INIT            initialise an FEC state (buf, 1)
 * ENCODE_ECC          add the FEC to the frame's 49 parameter bits
 * FEC_DEC             correct the 72 on-air bits back to 49, with a status
 * FEC_STATS           read the FEC error statistics
 * DEC_SETSTATS        hand them to the decoder
 */

/* State buffer sizes, from the donor init routines' clear/write extents. */
#define ENC_STATE_SIZE 0x2000
#define DEC_STATE_SIZE 0x07EC
#define ECC_STATE_SIZE 0x0100
#define FEC_STATS_SIZE 0x0034

/*
 * Decode mode word of the priming call the donor makes once after setup, which
 * leaves the decoder in its idle state before the first real frame.
 */
#define DEC_MODE_PRIME 8

/*
 * Mode word passed to the encoder for both 80-sample halves of a frame, as the
 * donor's own encode driver does once its setup has run.
 */
#define ENC_MODE 0x0800

/*
 * Encode the microphone through the vocoder on transmit. Set to 0 to send a
 * valid AMBE silence frame instead, which keeps the rest of the transmit path
 * working while the vocoder is left out, for instance to tell an encode
 * problem from an RF one.
 */
#define DMR_CODEC_TX_ENABLED 1

/* Function-pointer views of the codec entries. */
typedef void (*ambe_init_enc_fn)(void *st, int a, int b);
typedef void (*ambe_init_dec_fn)(void *st, int a);
typedef void (*ambe_cfg_enc_fn)(void *st, int, int, int, int, int, int, int,
                                int, int, int);
typedef void (*ambe_cfg_fn)(void *st, int a, int b);
typedef void (*ambe_encode_fn)(void *bits, int, void *wave, int count, int mode,
                               int half, int, void *st);
typedef void (*ambe_ecc_fn)(void *bits, void *bits2, int zero, void *ecc);
typedef void (*ambe_decode_fn)(void *wave, int count, void *bits, int, int,
                               int half, void *st);
typedef int (*ambe_ecc_init_fn)(void *ecc, int a);
typedef int (*ambe_fec_dec_fn)(void *out, int, void *in, int, int, void *ecc);
typedef void (*ambe_fec_stats_fn)(void *stats, void *ecc);
typedef void (*ambe_setstats_fn)(void *st, void *stats);

static uint8_t encState[ENC_STATE_SIZE] __attribute__((aligned(4)));
static uint8_t decState[DEC_STATE_SIZE] __attribute__((aligned(4)));
static uint8_t encEccState[ECC_STATE_SIZE] __attribute__((aligned(4)));
static uint8_t decEccState[ECC_STATE_SIZE] __attribute__((aligned(4)));
static uint8_t fecStats[FEC_STATS_SIZE] __attribute__((aligned(4)));
static bool ready = false;

#if defined(PLATFORM_CS7000P)
/**
 * CRC32 as zlib computes it, four bits at a time to keep the table small.
 *
 * @param data: data to check.
 * @param len: its length, in bytes.
 * @return the CRC32.
 */
static uint32_t crc32(const uint8_t *data, size_t len)
{
    static const uint32_t table[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
        0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
        0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C
    };
    uint32_t crc = 0xFFFFFFFF;

    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        crc = (crc >> 4) ^ table[crc & 0x0F];
        crc = (crc >> 4) ^ table[crc & 0x0F];
    }

    return ~crc;
}
#endif

bool dmr_codec_available(void)
{
#if defined(PLATFORM_CS7000P)
    /*
     * Another stock firmware version keeps the codec at other addresses, so the
     * whole region is checked. It is read once, the answer kept.
     */
    static uint8_t found = 0; // 0 until checked, then 1 if found, 2 if not

    if (found == 0) {
        const uint8_t *region = (const uint8_t *)AMBE_REGION_START;
        size_t len = AMBE_REGION_END - AMBE_REGION_START;
        found = (crc32(region, len) == AMBE_REGION_CRC32) ? 1 : 2;
    }

    return found == 1;
#else
    return *(const volatile uint32_t *)AMBE_REGION_START == AMBE_BLOB_MAGIC;
#endif
}

bool dmr_codec_init(void)
{
    if (dmr_codec_available() == false) {
        ready = false;
        return false;
    }

    memset(encState, 0, sizeof(encState));
    memset(decState, 0, sizeof(decState));
    memset(encEccState, 0, sizeof(encEccState));
    memset(decEccState, 0, sizeof(decEccState));

    /*
     * The donor firmware's setup runs this exact sequence before the first
     * encode/decode: bulk-init the encoder, the decoder and their FEC states,
     * then apply the fixed configuration and prime the decoder once. The
     * parameters are the constants the donor uses; they are rate/scale
     * settings, not codec data. An FEC state which was never initialised
     * leaves the encoder's output unprotected.
     */
    ((ambe_init_enc_fn)AMBE_INIT_ENC)(encState, 1, 1);
    ((ambe_ecc_init_fn)AMBE_ECC_INIT)(encEccState, 1);
    ((ambe_init_dec_fn)AMBE_INIT_DEC)(decState, 1);
    ((ambe_ecc_init_fn)AMBE_ECC_INIT)(decEccState, 1);
    ((ambe_cfg_enc_fn)AMBE_CFG_ENC)(encState, -22, 20, -10, -40, 0x4000, 0xCCC,
                                    0xCCC, 0x51E, 10, 20);
    ((ambe_cfg_fn)AMBE_CFG_A)(encState, 0x80, -20);
    ((ambe_cfg_fn)AMBE_CFG_B)(decState, 0x80, -20);

    int16_t primePcm[DMR_CODEC_PCM_SAMPLES / 2];
    uint16_t primeBits[49];
    memset(primeBits, 0, sizeof(primeBits));
    ((ambe_decode_fn)AMBE_DECODE)(primePcm, 0x50, primeBits, 0, DEC_MODE_PRIME,
                                  0, decState);

    ready = true;
    return true;
}

void dmr_codec_terminate(void)
{
    ready = false;
}

/*
 * A single 9-byte AMBE silence frame (the standard public pattern). Emitted on
 * TX while the encode path is gated off, and used by the decode self-test.
 */
static const uint8_t ambeSilenceFrame[DMR_CODEC_FRAME_BYTES] = {
    0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B
};

void dmr_codec_encode(uint8_t frame[DMR_CODEC_FRAME_BYTES],
                      const int16_t pcm[DMR_CODEC_PCM_SAMPLES])
{
    /*
     * One bit per 16-bit word: the encoder leaves the 49 parameter bits here and
     * the FEC stage expands them in place to the 72 protected on-air bits.
     */
    uint16_t bits[72];

    /* With encode switched off, transmit valid silence (DMR_CODEC_TX_ENABLED). */
    if (DMR_CODEC_TX_ENABLED == 0) {
        memcpy(frame, ambeSilenceFrame, DMR_CODEC_FRAME_BYTES);
        (void)pcm;
        return;
    }

    memset(frame, 0, DMR_CODEC_FRAME_BYTES);
    if (ready == false)
        return;

    memset(bits, 0, sizeof(bits));

    /*
     * A 20 ms frame is encoded in two 80-sample halves, then protected. The
     * encoder keeps the pitch history in its state across the two calls and
     * writes the frame's parameter bits on the second one; the half flag tells
     * them apart. The FEC stage copies its input before writing, so it runs in
     * place.
     */
    ((ambe_encode_fn)AMBE_ENCODE)(bits, 0, (void *)pcm, 80, ENC_MODE, 0, 0x2000,
                                  encState);
    ((ambe_encode_fn)AMBE_ENCODE)(bits, 0, (void *)(pcm + 80), 80, ENC_MODE, 1,
                                  0x2000, encState);
    ((ambe_ecc_fn)AMBE_ENCODE_ECC)(bits, bits, 0, encEccState);

    /* Pack the 72 protected bits MSB first into the 9-byte AMBE frame. */
    for (int i = 0; i < 72; i++) {
        if (bits[i] & 1)
            frame[i >> 3] |= 0x80 >> (i & 7);
    }
}

void dmr_codec_decode(int16_t pcm[DMR_CODEC_PCM_SAMPLES],
                      const uint8_t frame[DMR_CODEC_FRAME_BYTES])
{
    uint16_t bits[72];

    memset(pcm, 0, DMR_CODEC_PCM_SAMPLES * sizeof(int16_t));
    if (ready == false)
        return;

    /* One on-air bit per word, MSB first, as the donor feeds its decoder. */
    for (int i = 0; i < 72; i++)
        bits[i] = (frame[i >> 3] >> (7 - (i & 7))) & 1;

    /*
     * De-interleave, de-scramble and correct the frame in place, leaving the 49
     * parameter bits. The returned status tells the decoder how damaged the
     * frame was, so it can repeat or mute it; the error statistics feed the
     * decoder's own concealment.
     */
    int status = ((ambe_fec_dec_fn)AMBE_FEC_DEC)(bits, 0, bits, 1, 0,
                                                 decEccState)
               & 0xFFFF;
    ((ambe_fec_stats_fn)AMBE_FEC_STATS)(fecStats, decEccState);
    ((ambe_setstats_fn)AMBE_DEC_SETSTATS)(decState, fecStats);

    /* Two 80-sample halves per frame, decoder state carried across the pair. */
    ((ambe_decode_fn)AMBE_DECODE)(pcm, 0x50, bits, 0, status, 0, decState);
    ((ambe_decode_fn)AMBE_DECODE)(pcm + 80, 0x50, bits, 0, status, 1, decState);
}

/* One cycle of a 1 kHz tone sampled at 8 kHz, the encode test input. */
static const int16_t selfTestTone[8] = { 0, 5657,  8000,  5657,
                                         0, -5657, -8000, -5657 };

#define SELFTEST_FRAMES 4

static int16_t selfTestPcm[DMR_CODEC_PCM_SAMPLES];
static int16_t selfTestOut[DMR_CODEC_PCM_SAMPLES];

/**
 * Return the peak absolute sample of the decoded PCM, a cheap health metric.
 */
static int16_t pcmPeak(const int16_t *pcm)
{
    int16_t peak = 0;
    for (int i = 0; i < DMR_CODEC_PCM_SAMPLES; i++) {
        int16_t a = (pcm[i] < 0) ? (int16_t)-pcm[i] : pcm[i];
        if (a > peak)
            peak = a;
    }
    return peak;
}

/**
 * The self-test proper, run on its own thread by dmr_codec_selfTest().
 */
static void *selfTestThread(void *arg)
{
    (void)arg;

    if (dmr_codec_available() == false) {
        dmrDebug_log(DMR_DBG_INFO, "codec: not found at 0x%08lX",
                     (unsigned long)AMBE_REGION_START);
        return NULL;
    }

    dmrDebug_log(DMR_DBG_INFO, "codec: found, self-test start");
    if (dmr_codec_init() == false) {
        dmrDebug_log(DMR_DBG_INFO, "codec: init failed");
        return NULL;
    }
    dmrDebug_log(DMR_DBG_INFO, "codec: init ok");

    /* Decode path: decoding the silence frame must come back as near silence. */
    dmr_codec_decode(selfTestPcm, ambeSilenceFrame);
    dmrDebug_log(DMR_DBG_INFO, "codec: decode silence peak %d (expect ~0)",
                 pcmPeak(selfTestPcm));

    if (DMR_CODEC_TX_ENABLED == 0) {
        dmrDebug_log(DMR_DBG_INFO, "codec: encode switched off, "
                                   "decode-only self-test done");
        return NULL;
    }

    /*
     * Encode path: encode a few frames of the tone so the encoder's inter-frame
     * state settles, streaming each 9-byte AMBE frame as a "$CE" record. An
     * off-radio decoder (mbelib) turns these back into audio to confirm the
     * encode is producing valid AMBE.
     */
    uint8_t frame[DMR_CODEC_FRAME_BYTES];
    for (int i = 0; i < DMR_CODEC_PCM_SAMPLES; i++)
        selfTestPcm[i] = selfTestTone[i & 7];

    for (int f = 0; f < SELFTEST_FRAMES; f++) {
        dmr_codec_encode(frame, selfTestPcm);
        dmrDebug_record(DMR_DBG_INFO, "CE", frame, sizeof(frame));
    }
    dmrDebug_log(DMR_DBG_INFO, "codec: encoded %d tone frames",
                 SELFTEST_FRAMES);

    /* Round-trip one frame through encode then decode; a tone in should give a
     * loud, tone-like output. */
    dmr_codec_encode(frame, selfTestPcm);
    dmr_codec_decode(selfTestOut, frame);
    dmrDebug_log(DMR_DBG_INFO, "codec: self-test done, round-trip peak %d",
                 pcmPeak(selfTestOut));
    return NULL;
}

/*
 * Stack for the self-test thread. The encoder alone needs about 4 kB of stack,
 * more than the thread serving the debug tap has to spare.
 */
#define SELFTEST_STACK_SIZE 8192

void dmr_codec_selfTest(void)
{
    pthread_attr_t attr;
    pthread_t thread;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, SELFTEST_STACK_SIZE);

    if (pthread_create(&thread, &attr, selfTestThread, NULL) != 0) {
        dmrDebug_log(DMR_DBG_INFO, "codec: cannot start the self-test thread");
        return;
    }

    pthread_join(thread, NULL);
}
