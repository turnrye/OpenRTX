/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * ui_shim.cpp -- the C ABI boundary of the "ortx" UI toolkit.
 *
 * Implements the eight functions of core/ui.h (the frozen seam the rest of the
 * firmware calls, only from threads.c / openrtx.c / state.c) and forwards into
 * the C++ retained-widget toolkit. threads.c, graphics.c, the state module,
 * the event queue and the sync_rtx/mutex discipline are all unchanged.
 *
 * The active screen is the top of a Navigator's View stack (each View driving a
 * Screen). Input events are decoded and dispatched into the active view, which
 * returns a navigation intent (push a child screen / pop back); drawing pulls
 * from the state snapshot and repaints only when something actually changed.
 * The VFO home, a main menu and a settings submenu exist so far; remaining
 * views land in later slices.
 */

#include "core/ui.h"
#include "core/state.h"
#include "core/event.h"
#include "core/graphics.h"
#include "hwconfig.h"

#include "core/Event.hpp"
#include "core/Navigator.hpp"
#include "render/DrawCtx.hpp"
#include "style/SemanticColor.hpp"
#include "views/VfoView.hpp"
#include "views/MenuView.hpp"
#include "views/InfoView.hpp"
#include "views/AboutView.hpp"
#include "views/DisplayView.hpp"
#include "views/ChecklistView.hpp"

#include <cstdint>
#include <cstring>

using namespace ortxui;

namespace
{

/* Classic UI event ring: single-thread producer/consumer (all in the UI
 * thread), so no locking is required. */
constexpr uint8_t MAX_NUM_EVENTS = 16;

event_t evQueue[MAX_NUM_EVENTS];
uint8_t evQueue_rdPos = 0;
uint8_t evQueue_wrPos = 0;

/* UI-thread-local snapshot of the radio state. ui_saveState() copies the
 * global `state` here under state_mutex; drawing reads only this snapshot. */
state_t last_state;

/* Menu item tables (borrowed by the MenuViews; must outlive them, so static).
 * Content mirrors the classic UI's menus; the config guards keep parity with
 * targets that omit GPS/RTC/M17. */
const char *const kMainMenu[] = {
    "Banks",    "Channels", "Contacts",
#ifdef CONFIG_GPS
    "GPS",
#endif
    "Settings", "Info",     "About",
};
constexpr uint16_t kMainMenuCount = sizeof(kMainMenu) / sizeof(kMainMenu[0]);

const char *const kSettingsMenu[] = {
    "Display",
#ifdef CONFIG_RTC
    "Time & Date",
#endif
#ifdef CONFIG_GPS
    "GPS",
#endif
    "Radio",
#ifdef CONFIG_M17
    "M17",
#endif
    "FM",          "Accessibility", "Default Settings",
};
constexpr uint16_t kSettingsMenuCount = sizeof(kSettingsMenu)
                                      / sizeof(kSettingsMenu[0]);

/* The view singletons and the Navigator whose stack top is the active screen.
 * Each View owns its widget tree in static storage. */
VfoView vfoView;
MenuView mainMenu;
MenuView settingsMenu;
InfoView infoView;
AboutView aboutView;
DisplayView displayView;
ChecklistView checklistView;
Navigator nav;

/* Wire the row of `menu` whose label matches `label` (from `table`) to `target`. */
void wireRow(MenuView &menu, const char *const *table, uint16_t count,
             const char *label, View *target)
{
    for (uint16_t i = 0; i < count; i++) {
        if (strcmp(table[i], label) == 0) {
            menu.setRowTarget(i, target);
            return;
        }
    }
}

} // namespace

extern "C" void ui_init()
{
    evQueue_rdPos = 0;
    evQueue_wrPos = 0;
    last_state = state;

    vfoView.build();
    mainMenu.build("Menu", kMainMenu, kMainMenuCount);
    settingsMenu.build("Settings", kSettingsMenu, kSettingsMenuCount);
    infoView.build();
    aboutView.build();
    displayView.build();
    checklistView.build();

    /* Wire menu rows to their views. Rows resolve by label because the indices
     * shift with the config guards on the menu tables. */
    wireRow(mainMenu, kMainMenu, kMainMenuCount, "Settings", &settingsMenu);
    wireRow(mainMenu, kMainMenu, kMainMenuCount, "Info", &infoView);
    wireRow(mainMenu, kMainMenu, kMainMenuCount, "About", &aboutView);
    wireRow(settingsMenu, kSettingsMenu, kSettingsMenuCount, "Display",
            &displayView);
    wireRow(settingsMenu, kSettingsMenu, kSettingsMenuCount, "Accessibility",
            &checklistView);

    vfoView.setMenu(&mainMenu);
    nav.setRoot(&vfoView);

    state.ui_screen = 0; /* MAIN_VFO */
}

extern "C" void ui_drawSplashScreen()
{
    DrawCtx d;
    d.clearScreen(Sem::Background);

    const Point logo = { 0, (int16_t)((CONFIG_SCREEN_HEIGHT / 2) - 6) };
    const Point call = { 0, (int16_t)(CONFIG_SCREEN_HEIGHT - 8) };
    d.text(logo, FONT_SIZE_12PT, TEXT_ALIGN_CENTER, Sem::Primary, "OPN\nRTX");
    d.text(call, FONT_SIZE_8PT, TEXT_ALIGN_CENTER, Sem::OnSurface,
           state.settings.callsign);
}

extern "C" void ui_saveState()
{
    last_state = state;
}

extern "C" void ui_updateFSM(bool *sync_rtx)
{
    (void)sync_rtx;

    if (evQueue_rdPos == evQueue_wrPos)
        return;

    /* Pop one event per tick, matching the classic loop cadence. */
    const event_t raw = evQueue[evQueue_rdPos];
    evQueue_rdPos = (uint8_t)((evQueue_rdPos + 1) % MAX_NUM_EVENTS);

    const Event ev = Event::decode((uint8_t)raw.type, raw.payload);
    nav.dispatch(ev);
}

extern "C" bool ui_updateGUI()
{
    View *view = nav.active();
    if (view == nullptr)
        return false;

    view->syncFromState(last_state);

    Screen &screen = view->screen();
    if (!screen.dirty())
        return false;

    DrawCtx d;
    d.clearScreen(Sem::Background);
    screen.paintTree(d);
    screen.clearDirty();
    return true;
}

extern "C" bool ui_pushEvent(const uint8_t type, const uint32_t data)
{
    const uint8_t newWrPos = (uint8_t)((evQueue_wrPos + 1) % MAX_NUM_EVENTS);
    if (newWrPos == evQueue_rdPos)
        return false; /* queue full */

    event_t event;
    event.value = 0;
    event.type = type;
    event.payload = data;
    evQueue[evQueue_wrPos] = event;
    evQueue_wrPos = newWrPos;
    return true;
}

extern "C" void ui_terminate()
{
}
