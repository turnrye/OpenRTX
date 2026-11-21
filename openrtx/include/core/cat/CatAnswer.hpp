/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CATANSWER_H
#define CATANSWER_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <array>
#include <cstddef>
#include <cstdint>

/**
 * Builds the answer to a CAT command from text and fixed-width numbers. Text
 * that does not fit is dropped, but there is always room for the ';' that
 * terminates the answer.
 */
class CatAnswer
{
public:
    /**
     * Constructor.
     */
    CatAnswer();

    /**
     * Destructor.
     */
    ~CatAnswer();

    /**
     * Append one character.
     *
     * @param c: character to append.
     */
    void append(const char c);

    /**
     * Append a string, up to its terminator or a maximum length.
     *
     * @param str: string to append.
     * @param maxLen: maximum number of characters to append.
     */
    void append(const char *str, const size_t maxLen);

    /**
     * Append a number in a fixed number of digits, zero-padded. Digits beyond
     * that number are dropped.
     *
     * @param value: number to append.
     * @param digits: number of digits.
     * @param base: number base, 10 or 16; hexadecimal digits are upper case.
     */
    void appendNumber(uint32_t value, const size_t digits,
                      const uint32_t base = 10);

    /**
     * Terminate the answer with ';'.
     */
    void finish();

    /**
     * Get the answer text.
     *
     * @return answer text, not null-terminated.
     */
    const char *data() const
    {
        return text.data();
    }

    /**
     * Get the answer length.
     *
     * @return number of characters, 0 for no answer.
     */
    size_t size() const
    {
        return length;
    }

private:
    static constexpr size_t MAX_LENGTH = 48; ///< Longest answer, ';' excluded.

    std::array<char, MAX_LENGTH + 1> text;   ///< Answer text, room for ';'.
    size_t length;                           ///< Characters in text.
};

#endif // CATANSWER_H
