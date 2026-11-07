/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include "protocols/APRS/symbols.h"
#include <cstring>

TEST_CASE("APRS symbols: every entry is valid and unique", "[aprs]")
{
    REQUIRE(aprsSymbolsNum > 0);

    for (size_t i = 0; i < aprsSymbolsNum; i++) {
        const aprsSymbol &s = aprsSymbols[i];

        INFO("entry " << i << ": " << s.name);
        CHECK(((s.table == '/') || (s.table == '\\')));
        CHECK(((s.code > ' ') && (s.code < 0x7f)));
        CHECK(strlen(s.name) > 0);
        CHECK(strlen(s.name) <= APRS_SYMBOL_NAME_MAX);
        CHECK(aprsSymbolFind(s.table, s.code) == (int)i);
    }
}

TEST_CASE("APRS symbols: lookup", "[aprs]")
{
    /* The beacon default is the first entry. */
    CHECK(aprsSymbolFind('/', '[') == 0);
    CHECK(strcmp(aprsSymbols[aprsSymbolFind('/', '>')].name, "Car") == 0);
    CHECK(aprsSymbolFind('\\', '[') == -1);
    CHECK(aprsSymbolFind('/', 'Z') == -1);
}
