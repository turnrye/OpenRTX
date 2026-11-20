/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/cat/cat.h"
#include "core/cat/CatAnswer.hpp"
#include "core/cat/CatParser.hpp"
#include "core/cat/commands.hpp"
#include "core/threads.h"
#include <algorithm>
#include <array>
#include <cerrno>

/**
 * A CAT command and its handler.
 */
struct CatCommand {
    char name[2];   ///< The two letters of the command.
    CatHandler run; ///< Handler.
};

// Every command the radio executes, in alphabetical order
static constexpr std::array<CatCommand, 27> commands = { {
    { { 'A', 'I' }, cat_cmdAI }, { { 'B', 'S' }, cat_cmdBS },
    { { 'C', 'N' }, cat_cmdCN }, { { 'C', 'T' }, cat_cmdCT },
    { { 'E', 'X' }, cat_cmdEX }, { { 'F', 'A' }, cat_cmdFA },
    { { 'F', 'B' }, cat_cmdFB }, { { 'F', 'T' }, cat_cmdFT },
    { { 'I', 'D' }, cat_cmdID }, { { 'I', 'F' }, cat_cmdIF },
    { { 'M', 'D' }, cat_cmdMD }, { { 'N', 'A' }, cat_cmdNA },
    { { 'O', 'S' }, cat_cmdOS }, { { 'P', 'C' }, cat_cmdPC },
    { { 'P', 'S' }, cat_cmdPS }, { { 'S', 'H' }, cat_cmdSH },
    { { 'S', 'M' }, cat_cmdSM }, { { 'S', 'Q' }, cat_cmdSQ },
    { { 'T', 'X' }, cat_cmdTX }, { { 'Z', 'C' }, cat_cmdZC },
    { { 'Z', 'D' }, cat_cmdZD }, { { 'Z', 'M' }, cat_cmdZM },
    { { 'Z', 'N' }, cat_cmdZN }, { { 'Z', 'O' }, cat_cmdZO },
    { { 'Z', 'P' }, cat_cmdZP }, { { 'Z', 'R' }, cat_cmdZR },
    { { 'Z', 'V' }, cat_cmdZV },
} };

// Bytes read from the link per call of cat_task()
static constexpr size_t RX_CHUNK = 64;

static const struct chardev_ops *device;
static CatParser parser;

/**
 * Execute one command and send its answer.
 *
 * @param cmd: command text, without its ';'.
 * @param len: command length.
 * @param now: current time in milliseconds.
 */
static void execute(const char *cmd, const size_t len, const long long now)
{
    static constexpr char ERROR[] = "?;";
    CatAnswer answer;
    int ret = -EINVAL;

    auto match = [cmd](const CatCommand &c) {
        return (c.name[0] == cmd[0]) && (c.name[1] == cmd[1]);
    };

    if (len >= 2) {
        auto it = std::find_if(commands.begin(), commands.end(), match);
        if (it != commands.end()) {
            CatRequest req = { &cmd[2], len - 2, now };
            ret = it->run(req, answer);
        }
    }

    if (ret < 0) {
        device->write(ERROR, sizeof(ERROR) - 1);
    } else if (answer.size() > 0) {
        answer.finish();
        device->write(answer.data(), answer.size());
    }
}

bool cat_parseNumber(const char *text, const size_t len, uint32_t &value)
{
    if ((len == 0) || (len > 9))
        return false;

    auto isDigit = [](const char c) { return (c >= '0') && (c <= '9'); };
    if (std::all_of(text, text + len, isDigit) == false)
        return false;

    value = 0;
    for (size_t i = 0; i < len; i++)
        value = (value * 10) + (text[i] - '0');

    return true;
}

void cat_stateChanged()
{
    threads_requestRtxSync();
}

void cat_channelChanged(state_t &s, const bool tuning)
{
    if (tuning)
        s.tuner_mode = VFO;

    if (s.tuner_mode == VFO)
        s.vfo_channel = s.channel;

    cat_stateChanged();
}

int cat_init(const struct chardev_ops *dev)
{
    if (dev->init != nullptr) {
        int ret = dev->init();
        if (ret < 0)
            return ret;
    }

    parser.reset();
    device = dev;
    return 0;
}

void cat_task(const long long now)
{
    std::array<char, RX_CHUNK> rx;

    // Not started, or its device failed to start
    if (device == nullptr)
        return;

    ssize_t n = device->read(rx.data(), rx.size());
    for (ssize_t i = 0; i < n; i++) {
        if (parser.push(rx[i]))
            execute(parser.data(), parser.size(), now);
    }
}
