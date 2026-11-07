/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APRS_SYMBOLS_H
#define APRS_SYMBOLS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief An APRS symbol: its table, its code and a short name to show.
 */
struct aprsSymbol {
    char table;       /**< '/' primary or '\\' alternate table. */
    char code;        /**< Symbol code within the table.        */
    const char *name; /**< At most APRS_SYMBOL_NAME_MAX chars.  */
};

/** @brief Longest symbol name, so it fits beside a menu label. */
#define APRS_SYMBOL_NAME_MAX 10

/**
 * @brief The symbols offered for a beacon, most likely first.
 */
extern const struct aprsSymbol aprsSymbols[];

/** @brief Number of entries in aprsSymbols. */
extern const size_t aprsSymbolsNum;

/**
 * @brief Find a symbol in aprsSymbols.
 *
 * @param table: symbol table character.
 * @param code: symbol code.
 * @return index into aprsSymbols, or -1 if the symbol is not offered.
 */
int aprsSymbolFind(char table, char code);

#ifdef __cplusplus
}
#endif

#endif /* APRS_SYMBOLS_H */
