/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/cat/CatParser.hpp"

CatParser::CatParser()
{
    reset();
}

CatParser::~CatParser()
{
}

void CatParser::reset()
{
    length = 0;
    overflow = false;
    complete = false;
}

bool CatParser::push(const char c)
{
    if (complete)
        reset();

    if (c == ';') {
        bool empty = (length == 0) && (overflow == false);
        if (empty)
            return false;

        // An overlong command is reported as empty, so it answers an error
        if (overflow)
            length = 0;

        complete = true;
        return true;
    }

    bool lineEnd = (c == '\r') || (c == '\n');
    if (lineEnd)
        return false;

    bool printable = (c >= ' ') && (c <= '~');
    if (printable == false) {
        reset();
        return false;
    }

    if (length < command.size())
        command[length++] = c;
    else
        overflow = true;

    return false;
}
