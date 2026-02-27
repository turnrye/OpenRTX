/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>

extern "C" {
#include "protocols/M17/T9.h"
}
#include "protocols/M17/dict_en.h"

TEST_CASE("getDigit maps letters to correct T9 digits", "[t9]")
{
    /* T9 layout:
     * 2 = abc, 3 = def, 4 = ghi, 5 = jkl,
     * 6 = mno, 7 = pqrs, 8 = tuv, 9 = wxyz
     */
    struct {
        char letter;
        char expected;
    } cases[] = {
        { 'a', '2' }, { 'b', '2' }, { 'c', '2' }, { 'd', '3' }, { 'e', '3' },
        { 'f', '3' }, { 'g', '4' }, { 'h', '4' }, { 'i', '4' }, { 'j', '5' },
        { 'k', '5' }, { 'l', '5' }, { 'm', '6' }, { 'n', '6' }, { 'o', '6' },
        { 'p', '7' }, { 'q', '7' }, { 'r', '7' }, { 's', '7' }, { 't', '8' },
        { 'u', '8' }, { 'v', '8' }, { 'w', '9' }, { 'x', '9' }, { 'y', '9' },
        { 'z', '9' },
    };

    int n = sizeof(cases) / sizeof(cases[0]);
    for (int i = 0; i < n; i++) {
        REQUIRE(getDigit(cases[i].letter) == (uint8_t)cases[i].expected);
    }
}

TEST_CASE("getWord finds hello for code 43556", "[t9]")
{
    char code[] = "43556";
    char *result = getWord(dict_en, code);

    REQUIRE(result != NULL);
    REQUIRE(strcmp(result, "hello") == 0);
}

TEST_CASE("getWord finds the for code 843", "[t9]")
{
    char code[] = "843";
    char *result = getWord(dict_en, code);

    REQUIRE(result != NULL);
    REQUIRE(strcmp(result, "the") == 0);
}

TEST_CASE("Asterisk selects alternative matches", "[t9]")
{
    char code1[] = "843";
    char code2[] = "843*";

    char *first = getWord(dict_en, code1);
    char *second = getWord(dict_en, code2);

    REQUIRE(first != NULL);
    REQUIRE(strlen(first) > 0);
    REQUIRE(second != NULL);
    REQUIRE(strlen(second) > 0);
    REQUIRE(strcmp(first, second) != 0);

    /* Both should be 3 letters long (matching code length minus asterisks) */
    REQUIRE(strlen(second) == 3);
}

TEST_CASE("Unmatched code returns empty string", "[t9]")
{
    /* 99999999 - very unlikely to match any word */
    char code[] = "99999999";
    char *result = getWord(dict_en, code);

    REQUIRE(result != NULL);
    REQUIRE(strlen(result) == 0);
}

TEST_CASE("Single-letter lookup", "[t9]")
{
    char code[] = "4";
    char *result = getWord(dict_en, code);

    REQUIRE(result != NULL);
    REQUIRE(strlen(result) == 1);

    /* The digit for the returned letter should match the code */
    REQUIRE(getDigit(result[0]) == '4');
}

TEST_CASE("Found word digits are consistent with code", "[t9]")
{
    /* "968" = w/x/y, m/n/o, t/u/v - should find "you" or similar */
    char code[] = "968";
    char *result = getWord(dict_en, code);

    REQUIRE(result != NULL);
    REQUIRE(strlen(result) > 0);

    for (size_t i = 0; i < strlen(result); i++) {
        REQUIRE(getDigit(result[i]) == (uint8_t)code[i]);
    }
}
