/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The default user interface driven by key presses, as the keyboard scan hands
 * them over, checking what they change in the radio state.
 */

#include <catch2/catch_test_macros.hpp>
#include <initializer_list>

extern "C" {
#include "core/event.h"
#include "core/input.h"
#include "core/state.h"
#include "core/ui.h"
#include "interfaces/keyboard.h"
#include "ui/ui_default.h"
}

extern state_t state;

/* Start the user interface afresh on a screen. */
static void startOn(const uint8_t screen)
{
    ui_init();
    state.charge = 100;
    state.emergency = false;
    state.ui_screen = screen;
}

/* Press and release a key, and let the user interface take it. */
static bool press(const uint32_t keys, const bool longPress = false)
{
    kbd_msg_t msg;
    msg.value = 0;
    msg.keys = keys;
    msg.long_press = longPress ? 1 : 0;
    ui_pushEvent(EVENT_KBD, msg.value);

    bool sync = false;
    ui_updateFSM(&sync);
    return sync;
}

static void pressAll(std::initializer_list<uint32_t> keys)
{
    for (uint32_t k : keys)
        press(k);
}

TEST_CASE("UI number entry takes back the digit a backspace announces", "[ui]")
{
    startOn(SETTINGS_DMR);
    state.settings.dmr_id = 0;

    // DMR ID, the first item: 1, 2, 3, back, 4
    pressAll({ KEY_ENTER, KEY_1, KEY_2, KEY_3, KEY_LEFT, KEY_4, KEY_ENTER });
    REQUIRE(state.settings.dmr_id == 124);
}

/* Start on a main screen, in DMR mode, sending to talkgroup 9. */
static void startDmr(const uint8_t screen)
{
    startOn(screen);
    state.channel.mode = OPMODE_DMR;
    state.settings.dmr_talkgroup = 9;
    state.tone_enabled = false;
}

TEST_CASE("UI DMR talkgroup is typed in from the main screens", "[ui][dmr]")
{
    for (uint8_t screen : { MAIN_VFO, MAIN_MEM }) {
        startDmr(screen);

        pressAll({ KEY_HASH, KEY_3, KEY_1, KEY_4, KEY_4, KEY_3, KEY_1 });
        REQUIRE(state.settings.dmr_talkgroup == 9); // Not until entered
        REQUIRE(press(KEY_ENTER));                  // The radio is told
        REQUIRE(state.settings.dmr_talkgroup == 314431);
        REQUIRE(state.ui_screen == screen);

        // The 1750 Hz tone of the hash key is for FM only
        REQUIRE_FALSE(state.tone_enabled);
    }
}

TEST_CASE("UI DMR talkgroup entry takes a digit back, or is given up",
          "[ui][dmr]")
{
    startDmr(MAIN_VFO);
    pressAll({ KEY_HASH, KEY_9, KEY_1, KEY_LEFT, KEY_2, KEY_ENTER });
    REQUIRE(state.settings.dmr_talkgroup == 92);

    pressAll({ KEY_HASH, KEY_1, KEY_2, KEY_ESC });
    REQUIRE(state.settings.dmr_talkgroup == 92);
    REQUIRE(state.ui_screen == MAIN_VFO);

    // Back to the VFO's own keys: a digit types a frequency in
    press(KEY_5);
    REQUIRE(state.ui_screen == MAIN_VFO_INPUT);
}

TEST_CASE("UI DMR talkgroup entry keeps the old one for no DMR address",
          "[ui][dmr]")
{
    startDmr(MAIN_VFO);

    // Nothing typed
    press(KEY_HASH);
    REQUIRE_FALSE(press(KEY_ENTER));
    REQUIRE(state.settings.dmr_talkgroup == 9);

    // Longer than 24 bits: 16777216
    pressAll({ KEY_HASH, KEY_1, KEY_6, KEY_7, KEY_7, KEY_7, KEY_2, KEY_1, KEY_6,
               KEY_ENTER });
    REQUIRE(state.settings.dmr_talkgroup == 9);

    // The largest one there is
    pressAll({ KEY_HASH, KEY_1, KEY_6, KEY_7, KEY_7, KEY_7, KEY_2, KEY_1, KEY_5,
               KEY_ENTER });
    REQUIRE(state.settings.dmr_talkgroup == 16777215);
}

TEST_CASE("UI hash key still sends the 1750 Hz tone in FM", "[ui]")
{
    startOn(MAIN_VFO);
    state.channel.mode = OPMODE_FM;
    state.tone_enabled = false;

    press(KEY_HASH);
    REQUIRE(state.tone_enabled);
}
