/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CAT_DEBUG_H
#define CAT_DEBUG_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include "core/cat/commands.hpp"
#include "interfaces/chardev.h"
#include <cstddef>
#include <cstdint>

/*
 * CAT debug commands, for bench testing: builds with CONFIG_CAT_DEBUG (meson
 * option cat_debug) add the ZZ command, which reaches the commands drivers
 * register for low-level access to their chips.
 *
 *   ZZ;            the registered commands, ZZ followed by their names
 *   ZZxx<params>;  debug command xx, two characters, answered like any other
 *                  command: a read answers ZZxx<values>;, a set is silent
 *
 * The radio also sends debug lines on its own (cat_debugEmit()), as
 * ZZ<line>; between answers, and ZZ!<count>; when it dropped lines because
 * the link was too slow.
 */

/**
 * A debug command a driver registers, reached as ZZ followed by its name.
 */
struct CatDebugCommand {
    char name[2];          ///< The two characters of the command.
    CatHandler run;        ///< Handler, given the parameters after the name.
    CatDebugCommand *next; ///< Next registered command, set on registration.
};

/**
 * Register a debug command. A command is registered once, and a name already
 * taken is ignored. Safe from any thread.
 *
 * @param cmd: command, with static storage duration.
 */
void cat_debugRegister(CatDebugCommand &cmd);

/**
 * Parse the space-separated hexadecimal numbers of a debug command.
 *
 * @param req: command, whose parameters are parsed.
 * @param values: parsed numbers.
 * @param max: size of values.
 * @return how many numbers req holds, or -EINVAL if they are malformed or more
 * than max.
 */
int cat_debugParseHex(const CatRequest &req, uint32_t *values,
                      const size_t max);

/**
 * Run a function on the RTX thread, which drives the radio chips, and wait for
 * it. Called from the CAT handlers only.
 *
 * @param fn: function to run.
 * @param ctx: parameter of fn.
 * @return what fn returned, or -ETIMEDOUT if the RTX thread did not run it.
 */
int cat_debugRunOnRtx(int (*fn)(void *ctx), void *ctx);

/**
 * Run a function on the RTX thread, as cat_debugRunOnRtx().
 *
 * @param fn: function or lambda taking no parameters and returning an int.
 * @return what fn returned, or -ETIMEDOUT if the RTX thread did not run it.
 */
template <typename Function>
int cat_debugOnRtx(Function fn)
{
    auto call = [](void *ctx) { return (*static_cast<Function *>(ctx))(); };

    return cat_debugRunOnRtx(call, &fn);
}

/**
 * Queue a debug line, sent as ZZ<line>; with the next answers. A line that
 * does not fit in the queue is dropped. Safe from any thread, not from
 * interrupts.
 *
 * @param line: text, starting with the name of the command it belongs to,
 * without ';'.
 * @param len: number of characters in line.
 */
void cat_debugEmit(const char *line, const size_t len);

/**
 * Send the queued debug lines. Called by cat_task().
 *
 * @param dev: device carrying CAT.
 */
void cat_debugFlush(const struct chardev_ops *dev);

/**
 * ZZ: debug commands.
 */
int cat_cmdZZ(const CatRequest &req, CatAnswer &answer);

#endif // CAT_DEBUG_H
