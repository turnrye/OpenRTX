/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>

#include "core/cat/CatAnswer.hpp"
#include "core/cat/CatParser.hpp"

/**
 * Text collected from a parser or an answer, null-terminated.
 */
using Text = std::array<char, 256>;

/**
 * Feed text to a parser and collect the commands it completes, each followed
 * by '|'.
 *
 * @param parser: parser under test.
 * @param text: bytes received from the link.
 * @return completed commands.
 */
static Text parse(CatParser &parser, const char *text)
{
    Text out = {};
    size_t len = 0;

    for (size_t i = 0; text[i] != '\0'; i++) {
        if (parser.push(text[i]) == false)
            continue;

        memcpy(&out[len], parser.data(), parser.size());
        len += parser.size();
        out[len++] = '|';
    }

    return out;
}

/**
 * Get the text of an answer.
 *
 * @param answer: answer.
 * @return answer text.
 */
static Text textOf(const CatAnswer &answer)
{
    Text out = {};

    memcpy(out.data(), answer.data(), answer.size());
    return out;
}

TEST_CASE("CAT parser splits commands on ';'", "[cat][parser]")
{
    CatParser parser;

    SECTION("Commands, empty commands and line ends")
    {
        REQUIRE(strcmp(parse(parser, ";;FA;\r\nMD0;ID").data(), "FA|MD0|")
                == 0);

        // The unfinished command carries on with the next bytes
        REQUIRE(strcmp(parse(parser, ";").data(), "ID|") == 0);
    }

    SECTION("A control byte discards the command in progress")
    {
        REQUIRE(strcmp(parse(parser, "I\001D;").data(), "D|") == 0);
    }

    SECTION("An overlong command completes empty")
    {
        char text[64];
        memset(text, 'A', 40);
        strcpy(&text[40], ";ID;");

        REQUIRE(strcmp(parse(parser, text).data(), "|ID|") == 0);
    }
}

TEST_CASE("CAT answer builds fixed-width fields", "[cat][answer]")
{
    CatAnswer answer;

    SECTION("Text and zero-padded numbers")
    {
        answer.append("FA", 2);
        answer.appendNumber(145500000, 9);
        answer.append('X');
        answer.appendNumber(7, 3);
        answer.finish();

        REQUIRE(strcmp(textOf(answer).data(), "FA145500000X007;") == 0);
    }

    SECTION("Digits beyond the width and text past the length are dropped")
    {
        answer.appendNumber(1234, 2);
        answer.append("ABCDEF", 3);
        answer.finish();

        REQUIRE(strcmp(textOf(answer).data(), "34ABC;") == 0);
    }

    SECTION("A full answer still ends with ';'")
    {
        for (int i = 0; i < 100; i++)
            answer.append('A');
        answer.finish();

        REQUIRE(answer.size() == 49);
        REQUIRE(answer.data()[48] == ';');
    }
}
