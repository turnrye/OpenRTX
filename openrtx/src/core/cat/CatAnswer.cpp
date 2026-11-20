/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/cat/CatAnswer.hpp"
#include <algorithm>
#include <cstring>

CatAnswer::CatAnswer() : length(0)
{
}

CatAnswer::~CatAnswer()
{
}

void CatAnswer::append(const char c)
{
    if (length < MAX_LENGTH)
        text[length++] = c;
}

void CatAnswer::append(const char *str, const size_t maxLen)
{
    size_t room = MAX_LENGTH - length;
    size_t count = std::min(strnlen(str, maxLen), room);

    std::copy_n(str, count, &text[length]);
    length += count;
}

void CatAnswer::appendNumber(uint32_t value, const size_t digits)
{
    size_t count = std::min(digits, MAX_LENGTH - length);

    // Fill from the last digit, then drop the ones that did not fit
    for (size_t i = digits; i > 0; i--) {
        if (i <= count)
            text[length + i - 1] = '0' + (value % 10);
        value /= 10;
    }

    length += count;
}

void CatAnswer::finish()
{
    text[length++] = ';';
}
