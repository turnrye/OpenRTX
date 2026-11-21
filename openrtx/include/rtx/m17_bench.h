/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef M17_BENCH_H
#define M17_BENCH_H

/*
 * TEST ONLY bench console on the USB serial port, see m17_trace.cpp.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Read and run the host's commands. Call from the UI thread, every loop.
 */
void m17bench_poll();

/**
 * @return true while the host keys the radio.
 */
bool m17bench_ptt();

/**
 * The modulation pair the host set, if any.
 *
 * @param mod1: HR_C6000 Mod1 (0x46).
 * @param mod2: HR_C6000 Mod2 (0x45).
 * @return true if the host set one.
 */
bool m17bench_mod(uint8_t *mod1, uint8_t *mod2);

/**
 * Report the modulation pair the radio takes from its calibration.
 *
 * @param mod1: HR_C6000 Mod1 (0x46).
 * @param mod2: HR_C6000 Mod2 (0x45).
 */
void m17bench_setCal(uint8_t mod1, uint8_t mod2);

/**
 * @return true when the host asks for the M17 receive phase inverted once more.
 */
bool m17bench_rxFlip();

/**
 * Report the M17 receive phase inversion in use.
 *
 * @param inverted: true when the demodulator inverts.
 */
void m17bench_noteRxPhase(bool inverted);

/**
 * @return true, once, after a command changed the settings.
 */
bool m17bench_takeSync();

/**
 * Config register writes to apply right after the symbol transmission set-up.
 *
 * @param idx: index of the write.
 * @param reg: register, filled in.
 * @param value: value, filled in.
 * @return true if there is a write at that index.
 */
bool m17bench_symReg(int idx, uint8_t *reg, uint8_t *value);

/**
 * Read the platform's calibration memory, if it has one.
 *
 * @param addr: start address.
 * @param buf: destination.
 * @param len: number of bytes.
 * @return zero on success, a negative error code otherwise.
 */
int m17bench_nvm(uint32_t addr, uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif

#if defined(__cplusplus) && defined(CONFIG_CAT_DEBUG)
#include "core/cat/debug.hpp"

/**
 * Run a console command as a CAT debug command and answer with its reply.
 *
 * @param word: the console's command word.
 * @param name: the debug command's two characters.
 * @param req: the debug command, whose parameters are the console arguments.
 * @param answer: the answer.
 * @return 0, or -EINVAL when the console replied ERR.
 */
int m17bench_cat(const char *word, const char *name, const CatRequest &req,
                 CatAnswer &answer);

/**
 * Register the debug commands on the bench state m17_trace.cpp holds: ZZTR,
 * ZZMO, ZZSY, ZZRF, ZZNV and ZZTK. Called by the radio driver of a bench
 * target.
 */
void m17bench_catRegister();

/**
 * Define the CAT debug command ZZ<c0><c1> for a console word, to register
 * with cat_debugRegister() from the driver the command pokes.
 */
#define M17BENCH_CAT_COMMAND(ident, word, c0, c1)                           \
    static int ident##Run(const CatRequest &req, CatAnswer &answer)        \
    {                                                                       \
        static const char name[2] = { c0, c1 };                             \
        return m17bench_cat(word, name, req, answer);                       \
    }                                                                       \
    static CatDebugCommand ident = { { c0, c1 }, ident##Run, nullptr };
#endif

#endif /* M17_BENCH_H */
