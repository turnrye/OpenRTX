/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * CAT commands of TX power and receiver levels.
 */

#include "core/cat/commands.hpp"
#include "core/utils.h"
#include <array>
#include <cerrno>

// Highest TX power PC sets, in watts
static constexpr uint32_t PC_MAX_WATTS = 100;

// Highest SQ value, OpenRTX's top squelch level
static constexpr uint32_t SQ_MAX = 100;
static constexpr uint32_t SQL_LEVEL_MAX = 15;

// SM value of each S-level from S0 to S9 + 20 dB, from the FT-991 calibration
// table of Hamlib (rigs/yaesu/ft991.h, FT991_STR_CAL)
static constexpr std::array<uint8_t, 12> S_METER = {
    0, 12, 27, 40, 55, 65, 80, 95, 112, 130, 150, 172,
};

int cat_cmdPC(const CatRequest &req, CatAnswer &answer)
{
    uint32_t watts;

    if (req.len == 0) {
        uint32_t power =
            cat_lockState([](const state_t &s) { return s.channel.power; });

        answer.append("PC", 2);
        answer.appendNumber((power + 500) / 1000, 3);
        return 0;
    }

    bool valid = (req.len == 3) && cat_parseNumber(req.args, req.len, watts)
              && (watts > 0) && (watts <= PC_MAX_WATTS);
    if (valid == false)
        return -EINVAL;

    return cat_editChannel(false, [watts](channel_t &ch) {
        ch.power = watts * 1000;
        return 0;
    });
}

int cat_cmdZP(const CatRequest &req, CatAnswer &answer)
{
    uint32_t mw;

    if (req.len == 0) {
        uint32_t power =
            cat_lockState([](const state_t &s) { return s.channel.power; });

        answer.append("ZP", 2);
        answer.appendNumber(power, 6);
        return 0;
    }

    bool valid = (req.len <= 6) && cat_parseNumber(req.args, req.len, mw)
              && (mw > 0);
    if (valid == false)
        return -EINVAL;

    return cat_editChannel(false, [mw](channel_t &ch) {
        ch.power = mw;
        return 0;
    });
}

int cat_cmdSQ(const CatRequest &req, CatAnswer &answer)
{
    uint32_t value;

    if ((req.len < 1) || (req.args[0] != '0'))
        return -EINVAL;

    if (req.len == 1) {
        uint32_t level =
            cat_lockState([](const state_t &s) { return s.settings.sqlLevel; });

        uint32_t percent = (level * SQ_MAX + SQL_LEVEL_MAX / 2) / SQL_LEVEL_MAX;

        answer.append("SQ0", 3);
        answer.appendNumber(percent, 3);
        return 0;
    }

    bool valid = (req.len == 4) && cat_parseNumber(&req.args[1], 3, value)
              && (value <= SQ_MAX);
    if (valid == false)
        return -EINVAL;

    uint8_t level = (value * SQL_LEVEL_MAX + SQ_MAX / 2) / SQ_MAX;
    cat_editSettings([level](settings_t &st) { st.sqlLevel = level; });
    return 0;
}

int cat_cmdSM(const CatRequest &req, CatAnswer &answer)
{
    if ((req.len != 1) || (req.args[0] != '0'))
        return -EINVAL;

    answer.append("SM0", 3);
    answer.appendNumber(S_METER[rssiToSlevel(state.rssi)], 3);
    return 0;
}
