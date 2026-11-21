/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/cat/cat.h"
#include "core/cat/debug.hpp"
#include "interfaces/delays.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <pthread.h>

/**
 * Debug lines waiting to be sent.
 */
struct EmitBuffer {
    // Room for a burst of bench records between two passes of cat_task()
    std::array<char, 4096> text; ///< Lines, each as ZZ<line>;.
    size_t length;              ///< Characters in text.
};

/**
 * State of the call cat_debugRunOnRtx() hands to the RTX thread.
 */
enum class RtxCall {
    IDLE,    ///< No call.
    QUEUED,  ///< Waiting for the RTX thread.
    RUNNING, ///< Running on the RTX thread.
    DONE,    ///< Returned, result in callResult.
};

// Longest parameters of a debug command
static constexpr size_t PARAMS_MAX = 32;

// Longest wait for the RTX thread to take a call, in milliseconds
static constexpr unsigned int RTX_TIMEOUT_MS = 500;

static std::atomic<CatDebugCommand *> registered{ nullptr };
static pthread_mutex_t registerMutex = PTHREAD_MUTEX_INITIALIZER;

// cat_debugEmit() fills one buffer while cat_debugFlush() sends the other
static std::array<EmitBuffer, 2> emitBuffers;
static size_t emitFilling;
static uint32_t emitDropped;
static pthread_mutex_t emitMutex = PTHREAD_MUTEX_INITIALIZER;

static std::atomic<RtxCall> callState{ RtxCall::IDLE };
static int (*callFunction)(void *ctx);
static void *callContext;
static int callResult;
static pthread_mutex_t callMutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * Find a registered debug command.
 *
 * @param name: the two characters of the command.
 * @return the command, or nullptr if none has that name.
 */
static CatDebugCommand *find(const char *name)
{
    CatDebugCommand *cmd = registered.load();

    while ((cmd != nullptr) && (std::equal(name, name + 2, cmd->name) == false))
        cmd = cmd->next;

    return cmd;
}

void cat_debugRegister(CatDebugCommand &cmd)
{
    pthread_mutex_lock(&registerMutex);

    if (find(cmd.name) == nullptr) {
        cmd.next = registered.load();
        registered.store(&cmd);
    }

    pthread_mutex_unlock(&registerMutex);
}

int cat_debugParseHex(const CatRequest &req, uint32_t *values, const size_t max)
{
    std::array<char, PARAMS_MAX + 1> text;
    size_t count = 0;

    if (req.len > PARAMS_MAX)
        return -EINVAL;

    std::copy_n(req.args, req.len, text.begin());
    text[req.len] = '\0';

    const char *pos = text.data();
    while (true) {
        while (*pos == ' ')
            pos++;

        if (*pos == '\0')
            break;

        // strtoul() would also take a sign
        if ((count == max) || (isxdigit(*pos) == 0))
            return -EINVAL;

        // At most 8 digits, as unsigned long may be wider than the value
        char *end;
        values[count++] = strtoul(pos, &end, 16);
        bool fits = (end - pos) <= 8;
        if ((fits == false) || ((*end != ' ') && (*end != '\0')))
            return -EINVAL;

        pos = end;
    }

    return count;
}

int cat_debugRunOnRtx(int (*fn)(void *ctx), void *ctx)
{
    pthread_mutex_lock(&callMutex);
    callFunction = fn;
    callContext = ctx;
    callState = RtxCall::QUEUED;
    pthread_mutex_unlock(&callMutex);

    for (unsigned int waited = 0;; waited++) {
        pthread_mutex_lock(&callMutex);
        RtxCall status = callState;
        bool timeout = (status == RtxCall::QUEUED)
                    && (waited >= RTX_TIMEOUT_MS);
        if ((status == RtxCall::DONE) || timeout)
            callState = RtxCall::IDLE;

        int ret = callResult;
        pthread_mutex_unlock(&callMutex);

        // A call already running is waited for, as it uses ctx
        if (status == RtxCall::DONE)
            return ret;

        if (timeout)
            return -ETIMEDOUT;

        sleepFor(0, 1);
    }
}

void cat_debugService()
{
    // Nothing queued, the common case: no lock on the RTX thread
    if (callState != RtxCall::QUEUED)
        return;

    pthread_mutex_lock(&callMutex);
    bool queued = (callState == RtxCall::QUEUED);
    if (queued)
        callState = RtxCall::RUNNING;

    pthread_mutex_unlock(&callMutex);

    if (queued == false)
        return;

    int ret = callFunction(callContext);

    pthread_mutex_lock(&callMutex);
    callResult = ret;
    callState = RtxCall::DONE;
    pthread_mutex_unlock(&callMutex);
}

void cat_debugEmit(const char *line, const size_t len)
{
    pthread_mutex_lock(&emitMutex);

    EmitBuffer &buf = emitBuffers[emitFilling];
    size_t needed = len + 3; // ZZ and ';'
    if ((buf.text.size() - buf.length) >= needed) {
        char *pos = &buf.text[buf.length];
        pos = std::copy_n("ZZ", 2, pos);
        pos = std::copy_n(line, len, pos);
        *pos = ';';
        buf.length += needed;
    } else {
        emitDropped++;
    }

    pthread_mutex_unlock(&emitMutex);
}

void cat_debugFlush(const struct chardev_ops *dev)
{
    pthread_mutex_lock(&emitMutex);
    EmitBuffer &buf = emitBuffers[emitFilling];
    emitFilling = 1 - emitFilling;
    uint32_t dropped = emitDropped;
    emitDropped = 0;
    pthread_mutex_unlock(&emitMutex);

    if (buf.length > 0)
        dev->write(buf.text.data(), buf.length);

    buf.length = 0;

    if (dropped > 0) {
        CatAnswer notice;
        notice.append("ZZ!", 3);
        notice.appendNumber(std::min<uint32_t>(dropped, 99999), 5);
        notice.finish();
        dev->write(notice.data(), notice.size());
    }
}

int cat_cmdZZ(const CatRequest &req, CatAnswer &answer)
{
    if (req.len == 0) {
        answer.append("ZZ", 2);
        for (auto cmd = registered.load(); cmd != nullptr; cmd = cmd->next)
            answer.append(cmd->name, 2);

        return 0;
    }

    if (req.len < 2)
        return -EINVAL;

    CatDebugCommand *cmd = find(req.args);
    if (cmd == nullptr)
        return -ENOTSUP;

    CatRequest params = { &req.args[2], req.len - 2, req.now };
    return cmd->run(params, answer);
}
