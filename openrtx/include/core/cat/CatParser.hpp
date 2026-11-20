/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CATPARSER_H
#define CATPARSER_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <array>
#include <cstddef>

/**
 * Splits the byte stream of a CAT link into commands. A command ends with
 * ';'; CR and LF are ignored, so commands can be typed in a terminal, and any
 * other control or non-ASCII byte discards the command in progress.
 */
class CatParser
{
public:
    /**
     * Constructor.
     */
    CatParser();

    /**
     * Destructor.
     */
    ~CatParser();

    /**
     * Discard the command in progress.
     */
    void reset();

    /**
     * Process one byte received from the link.
     *
     * @param c: received byte.
     * @return true if the byte completed a command, which data() and size()
     * then describe until the next call.
     */
    bool push(const char c);

    /**
     * Get the last completed command, without its ';'.
     *
     * @return command text, not null-terminated.
     */
    const char *data() const
    {
        return command.data();
    }

    /**
     * Get the length of the last completed command.
     *
     * @return number of characters, 0 for a command too long to execute.
     */
    size_t size() const
    {
        return length;
    }

private:
    static constexpr size_t MAX_LENGTH = 32; ///< Longest command, ';' excluded.

    std::array<char, MAX_LENGTH> command;    ///< Command text.
    size_t length;                           ///< Characters in command.
    bool overflow;                           ///< Longer than MAX_LENGTH.
    bool complete;                           ///< Ended by the last byte.
};

#endif // CATPARSER_H
