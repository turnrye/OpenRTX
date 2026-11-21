/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * CAT commands of the M17 settings: callsign, destination, channel access
 * number.
 */

#include "core/cat/commands.hpp"
#include "protocols/M17/Callsign.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>

using M17::Callsign;

// Highest M17 channel access number
static constexpr uint32_t CAN_MAX = 15;

/**
 * Text of an M17 callsign, null-terminated.
 */
using CallsignText = std::array<char, Callsign::MAX_CALLSIGN_CHARS + 1>;

/**
 * Convert text from the host to an M17 callsign, upper-cased.
 *
 * @param text: callsign text.
 * @param len: text length.
 * @param call: converted callsign.
 * @return true if the text is a valid callsign.
 */
static bool toCallsign(const char *text, const size_t len, CallsignText &call)
{
    auto upper = [](const char c) {
        bool lower = (c >= 'a') && (c <= 'z');
        return lower ? static_cast<char>(c - 'a' + 'A') : c;
    };

    if (len > Callsign::MAX_CALLSIGN_CHARS)
        return false;

    std::transform(text, text + len, call.begin(), upper);
    call[len] = '\0';

    return Callsign::isValid(call.data(), len);
}

int cat_cmdZC(const CatRequest &req, CatAnswer &answer)
{
    CallsignText call;

    if (req.len == 0) {
        call = cat_lockState([](const state_t &s) {
            CallsignText c;
            strncpy(c.data(), s.settings.callsign, c.size());
            return c;
        });

        answer.append("ZC", 2);
        answer.append(call.data(), Callsign::MAX_CALLSIGN_CHARS);
        return 0;
    }

    if (toCallsign(req.args, req.len, call) == false)
        return -EINVAL;

    cat_editSettings(
        [&call](settings_t &st) { strcpy(st.callsign, call.data()); });
    return 0;
}

int cat_cmdZD(const CatRequest &req, CatAnswer &answer)
{
    CallsignText dest;

    if (req.len == 0) {
        dest = cat_lockState([](const state_t &s) {
            CallsignText d;
            strncpy(d.data(), s.settings.m17_dest, d.size());
            return d;
        });

        // The broadcast is stored as an empty destination
        bool broadcast = (dest[0] == '\0');
        answer.append("ZD", 2);
        answer.append(broadcast ? Callsign::BROADCAST_CALL : dest.data(),
                      Callsign::MAX_CALLSIGN_CHARS);
        return 0;
    }

    if (toCallsign(req.args, req.len, dest) == false)
        return -EINVAL;

    if (strcmp(dest.data(), Callsign::BROADCAST_CALL) == 0)
        dest[0] = '\0';

    cat_editSettings(
        [&dest](settings_t &st) { strcpy(st.m17_dest, dest.data()); });
    return 0;
}

int cat_cmdZH(const CatRequest &req, CatAnswer &answer)
{
    if (req.len == 0) {
        uint32_t chip = cat_lockState(
            [](const state_t &s) { return s.settings.m17_chip_modem; });

        answer.append("ZH", 2);
        answer.appendNumber(chip, 1);
        return 0;
    }

    bool valid = (req.len == 1)
              && ((req.args[0] == '0') || (req.args[0] == '1'));
    if (valid == false)
        return -EINVAL;

    bool chip = (req.args[0] == '1');
    cat_editSettings([chip](settings_t &st) { st.m17_chip_modem = chip; });
    return 0;
}

int cat_cmdZN(const CatRequest &req, CatAnswer &answer)
{
    uint32_t can;

    if (req.len == 0) {
        can =
            cat_lockState([](const state_t &s) { return s.settings.m17_can; });

        answer.append("ZN", 2);
        answer.appendNumber(can, 2);
        return 0;
    }

    bool valid = (req.len == 2) && cat_parseNumber(req.args, req.len, can)
              && (can <= CAN_MAX);
    if (valid == false)
        return -EINVAL;

    cat_editSettings([can](settings_t &st) { st.m17_can = can; });
    return 0;
}
