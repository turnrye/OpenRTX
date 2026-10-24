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
