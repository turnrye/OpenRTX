/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstring>

#include "ui/T9InputControl.h"
#include "ui/UIContext.h"

extern "C" {
#include "core/input.h"
}

#ifdef CONFIG_UI_MODULE17
#include "ui/ui_mod17.h"
#else
#include "ui/ui_default.h"
#endif

extern layout_t layout;

// Helper: build an event_t carrying a keyboard message with the given key(s).
static event_t make_key_event(uint32_t keys)
{
    kbd_msg_t msg;
    msg.value = 0;
    msg.keys = keys;
    msg.long_press = 0;

    event_t ev;
    ev.type = EVENT_KBD;
    ev.payload = msg.value;
    return ev;
}

// ---- start() ----------------------------------------------------------------

TEST_CASE("start() clears buffer and places underscore cursor", "[ui][input]")
{
    UIContext ctx(layout);
    T9InputControl ctrl;

    char buf[16];
    memset(buf, 'X', sizeof(buf));
    ctrl.start(buf, 9, m17CallsignSymbols, FONT_SIZE_8PT);

    REQUIRE(buf[0] == '_');
    for (int i = 1; i <= 9; i++) {
        INFO("index " << i);
        REQUIRE(buf[i] == '\0');
    }
}

// ---- ENTER / ESC ------------------------------------------------------------

TEST_CASE("ENTER on empty buffer returns Confirmed and clears underscore",
          "[ui][input]")
{
    UIContext ctx(layout);
    T9InputControl ctrl;

    char buf[16];
    ctrl.start(buf, 9, m17CallsignSymbols, FONT_SIZE_8PT);

    InputResult r = ctrl.handleKey(ctx, make_key_event(KEY_ENTER));
    REQUIRE(r == InputResult::Confirmed);
    // The underscore cursor at position 0 should be stripped
    REQUIRE(buf[0] == '\0');
}

TEST_CASE("ESC returns Cancelled", "[ui][input]")
{
    UIContext ctx(layout);
    T9InputControl ctrl;

    char buf[16];
    ctrl.start(buf, 9, m17CallsignSymbols, FONT_SIZE_8PT);

    InputResult r = ctrl.handleKey(ctx, make_key_event(KEY_ESC));
    REQUIRE(r == InputResult::Cancelled);
}

// ---- T9 keypad input --------------------------------------------------------

#ifndef CONFIG_UI_MODULE17

TEST_CASE("First keypress places first character of key group", "[ui][input]")
{
    UIContext ctx(layout);
    T9InputControl ctrl;

    char buf[16];
    // m17CallsignSymbols key 2 = "ABC2"
    ctrl.start(buf, 9, m17CallsignSymbols, FONT_SIZE_8PT);

    InputResult r = ctrl.handleKey(ctx, make_key_event(KEY_2));
    REQUIRE(r == InputResult::Ongoing);
    REQUIRE(buf[0] == 'A');
}

TEST_CASE("Same key cycles through character set", "[ui][input]")
{
    UIContext ctx(layout);
    T9InputControl ctrl;

    char buf[16];
    // m17CallsignSymbols key 2 = "ABC2"
    ctrl.start(buf, 9, m17CallsignSymbols, FONT_SIZE_8PT);

    ctrl.handleKey(ctx, make_key_event(KEY_2));
    REQUIRE(buf[0] == 'A');

    ctrl.handleKey(ctx, make_key_event(KEY_2));
    REQUIRE(buf[0] == 'B');

    ctrl.handleKey(ctx, make_key_event(KEY_2));
    REQUIRE(buf[0] == 'C');

    ctrl.handleKey(ctx, make_key_event(KEY_2));
    REQUIRE(buf[0] == '2');

    // Wraps around
    ctrl.handleKey(ctx, make_key_event(KEY_2));
    REQUIRE(buf[0] == 'A');
}

TEST_CASE("Different key advances cursor position", "[ui][input]")
{
    UIContext ctx(layout);
    T9InputControl ctrl;

    char buf[16];
    ctrl.start(buf, 9, m17CallsignSymbols, FONT_SIZE_8PT);

    // Press KEY_2 → position 0 = 'A'
    ctrl.handleKey(ctx, make_key_event(KEY_2));

    // Press KEY_3 → position 1 = 'D' (first char of "DEF3")
    ctrl.handleKey(ctx, make_key_event(KEY_3));
    REQUIRE(buf[0] == 'A');
    REQUIRE(buf[1] == 'D');
}

TEST_CASE("ENTER confirms after typing characters", "[ui][input]")
{
    UIContext ctx(layout);
    T9InputControl ctrl;

    char buf[16];
    ctrl.start(buf, 9, m17CallsignSymbols, FONT_SIZE_8PT);

    ctrl.handleKey(ctx, make_key_event(KEY_2)); // A
    ctrl.handleKey(ctx, make_key_event(KEY_3)); // D

    InputResult r = ctrl.handleKey(ctx, make_key_event(KEY_ENTER));
    REQUIRE(r == InputResult::Confirmed);
    REQUIRE(buf[0] == 'A');
    REQUIRE(buf[1] == 'D');
    REQUIRE(buf[2] == '\0');
}

// ---- Delete -----------------------------------------------------------------

TEST_CASE("Arrow key deletes character at current position", "[ui][input]")
{
    UIContext ctx(layout);
    T9InputControl ctrl;

    char buf[16];
    ctrl.start(buf, 9, m17CallsignSymbols, FONT_SIZE_8PT);

    // Type two characters
    ctrl.handleKey(ctx, make_key_event(KEY_2)); // A at pos 0
    ctrl.handleKey(ctx, make_key_event(KEY_3)); // D at pos 1

    // Delete moves cursor back
    ctrl.handleKey(ctx, make_key_event(KEY_LEFT));
    REQUIRE(buf[1] == '\0');

    // Can still type again at position 0
    ctrl.handleKey(ctx, make_key_event(KEY_LEFT));
    REQUIRE(buf[0] == '\0');
}

TEST_CASE("Delete at position 0 resets keypress timer", "[ui][input]")
{
    UIContext ctx(layout);
    T9InputControl ctrl;

    char buf[16];
    ctrl.start(buf, 9, m17CallsignSymbols, FONT_SIZE_8PT);

    ctrl.handleKey(ctx, make_key_event(KEY_2)); // A at pos 0
    // Delete at pos 0
    ctrl.handleKey(ctx, make_key_event(KEY_UP));
    REQUIRE(buf[0] == '\0');

    // Next keypress should behave like first keypress (no cursor advance)
    ctrl.handleKey(ctx, make_key_event(KEY_3));
    REQUIRE(buf[0] == 'D');
    REQUIRE(buf[1] == '\0');
}

// ---- Max length boundary ----------------------------------------------------

TEST_CASE("Input stops at max length", "[ui][input]")
{
    UIContext ctx(layout);
    T9InputControl ctrl;

    char buf[4]; // maxLen=2 → buf must be at least 3 bytes
    ctrl.start(buf, 2, m17CallsignSymbols, FONT_SIZE_8PT);

    ctrl.handleKey(ctx, make_key_event(KEY_2)); // pos 0: A
    ctrl.handleKey(ctx, make_key_event(KEY_3)); // pos 1: D

    // Third different key should be rejected (pos 2 >= maxLen 2)
    InputResult r = ctrl.handleKey(ctx, make_key_event(KEY_4));
    REQUIRE(r == InputResult::Ongoing);

    // Buffer should be unchanged
    REQUIRE(buf[0] == 'A');
    REQUIRE(buf[1] == 'D');

    // Confirm should work
    r = ctrl.handleKey(ctx, make_key_event(KEY_ENTER));
    REQUIRE(r == InputResult::Confirmed);
    REQUIRE(buf[2] == '\0');
}

// ---- Empty key group --------------------------------------------------------

TEST_CASE("Key with empty symbol group is ignored", "[ui][input]")
{
    UIContext ctx(layout);
    T9InputControl ctrl;

    char buf[16];
    // m17CallsignSymbols key 11 (KEY_HASH) = "" (empty)
    ctrl.start(buf, 9, m17CallsignSymbols, FONT_SIZE_8PT);

    InputResult r = ctrl.handleKey(ctx, make_key_event(KEY_HASH));
    REQUIRE(r == InputResult::Ongoing);
    // Buffer still has underscore cursor, nothing typed
    REQUIRE(buf[0] == '_');
}

#endif // !CONFIG_UI_MODULE17
