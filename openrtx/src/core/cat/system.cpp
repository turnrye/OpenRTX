/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * CAT commands of the radio itself: identity, power, PTT, battery, firmware.
 */

#include "core/cat/commands.hpp"
#include "hwconfig.h"
#include "interfaces/platform.h"
#include "rtx/rtx.h"
#include <cerrno>

// Answer to ID: OpenRTX's own number, no Yaesu model's
static constexpr char RIG_ID[] = "0999";

// Longest transmission CAT holds the PTT for
static constexpr long long TX_TIMEOUT_MS = 120000;

int cat_cmdID(const CatRequest &req, CatAnswer &answer)
{
    if (req.len != 0)
        return -EINVAL;

    answer.append("ID", 2);
    answer.append(RIG_ID, sizeof(RIG_ID) - 1);
    return 0;
}

int cat_cmdAI(const CatRequest &req, CatAnswer &answer)
{
    if (req.len == 0) {
        answer.append("AI0", 3);
        return 0;
    }

    if ((req.len != 1) || (req.args[0] != '0'))
        return -ENOTSUP;

    return 0;
}

int cat_cmdPS(const CatRequest &req, CatAnswer &answer)
{
    if (req.len == 0) {
        answer.append("PS1", 3);
        return 0;
    }

    if ((req.len != 1) || (req.args[0] != '1'))
        return -ENOTSUP;

    return 0;
}

int cat_cmdTX(const CatRequest &req, CatAnswer &answer)
{
    if (req.len == 0) {
        char ptt = '0';
        if (platform_getPttStatus())
            ptt = '2';
        else if (rtx_pttActive())
            ptt = '1';

        answer.append("TX", 2);
        answer.append(ptt);
        return 0;
    }

    bool key = (req.args[0] == '1');
    if ((req.len != 1) || ((key == false) && (req.args[0] != '0')))
        return -EINVAL;

    if (key == false) {
        rtx_releasePtt();
        return 0;
    }

    // Refused where the radio's own PTT would not transmit either
    if (rtx_holdPtt(req.now + TX_TIMEOUT_MS) < 0)
        return -ENOTSUP;

    return 0;
}

int cat_cmdZR(const CatRequest &req, CatAnswer &answer)
{
    if (req.len != 0)
        return -EINVAL;

    answer.append("ZR", 2);
    answer.appendNumber(state.charge, 3);
    return 0;
}

int cat_cmdZV(const CatRequest &req, CatAnswer &answer)
{
    if (req.len != 0)
        return -EINVAL;

#ifdef GIT_VERSION
    static constexpr char VERSION[] = GIT_VERSION;

    answer.append("ZV", 2);
    answer.append(VERSION, sizeof(VERSION) - 1);
    return 0;
#else
    (void)answer;
    return -ENOTSUP;
#endif
}

int cat_cmdZB(const CatRequest &req, CatAnswer &answer)
{
    (void)answer;

    if ((req.len != 1) || (req.args[0] != '1'))
        return -EINVAL;

#ifdef CONFIG_BOOTLOADER_ENTRY
    platform_enterBootloader();
#endif

    return -ENOTSUP;
}
