/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_CHARSETS_HPP
#define ORTX_UI_CHARSETS_HPP

#include <cstdint>

namespace ortxui
{

/**
 * A character set ("keyboard") for the cursor-cycle text input. The platform
 * gives only a raw key bitmask (no T9 / tap-repeat), so text is entered by
 * cycling the character under the cursor through one of these ordered sets.
 * Space is first so a fresh slot reads as blank and cycles up into letters.
 */
struct Charset {
    const char *chars; //< ordered characters, NUL-terminated string body
    uint16_t len;      //< number of characters (excludes the NUL)
    const char *name;  //< short label (for a future keyboard-picker UI)

    /** Index of `c` in the set, or 0 (space) when absent. */
    int indexOf(char c) const
    {
        for (uint16_t i = 0; i < len; i++)
            if (chars[i] == c)
                return static_cast<int>(i);
        return 0;
    }

    /** Character at `i`, wrapping modulo the set length (handles cycling). */
    char at(int i) const
    {
        const int n = static_cast<int>(len);
        return chars[((i % n) + n) % n];
    }
};

/* Callsign / M17 destination: space + uppercase + digits + a few separators
 * (the historic callsign alphabet). namespace-scope constexpr has internal
 * linkage, so each translation unit gets its own read-only copy (no C++17
 * inline-variable dependency). */
constexpr char kCharsCallsign[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/.";

/* Free text (M17 meta, SMS): adds lowercase and common punctuation. */
constexpr char kCharsText[] =
    " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
    ".,?!-/@#'()&:;+*=";

/* Numeric / DTMF. */
constexpr char kCharsNumeric[] = "0123456789*#";

constexpr Charset CHARSET_CALLSIGN{ kCharsCallsign, sizeof(kCharsCallsign) - 1,
                                    "Callsign" };
constexpr Charset CHARSET_TEXT{ kCharsText, sizeof(kCharsText) - 1, "Text" };
constexpr Charset CHARSET_NUMERIC{ kCharsNumeric, sizeof(kCharsNumeric) - 1,
                                   "Numeric" };

} // namespace ortxui

#endif /* ORTX_UI_CHARSETS_HPP */
