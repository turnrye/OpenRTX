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
#include "core/input.h"
#include "core/graphics.h"
#include "interfaces/delays.h"
#include "interfaces/display.h"
#include "interfaces/keyboard.h"
#include "rtx/rtx.h"
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
#include "views/GpsView.hpp"
#include "views/GpsSettingsView.hpp"
#include "views/FmView.hpp"
#include "views/RadioView.hpp"
#include "views/M17View.hpp"
#include "views/DefaultsView.hpp"
#include "views/ChannelsView.hpp"
#include "views/ContactsView.hpp"
#include "views/BanksView.hpp"

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
FmView fmView;
RadioView radioView;
DefaultsView defaultsView;
ChannelsView channelsView;
ContactsView contactsView;
BanksView banksView;
#ifdef CONFIG_M17
M17View m17View;
#endif
#ifdef CONFIG_GPS
GpsView gpsView;
GpsSettingsView gpsSettingsView;
#endif
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

/* Display standby (backlight timeout) — parity with the classic default UI.
 * On `settings.display_timer` of idle the backlight is blanked and drawing is
 * suspended; any keypress (or RF/volume activity) wakes it again. */
bool standby = false;
long long last_event_tick = 0;

void enterStandby()
{
    if (standby)
        return;
    standby = true;
    display_setBacklightLevel(0);
}

/* Returns true if this call actually left standby (used to swallow the wake
 * keypress, matching the classic behaviour). */
bool exitStandby(long long now)
{
    last_event_tick = now;
    if (!standby)
        return false;
    standby = false;
    display_setBacklightLevel(state.settings.brightness);
    if (View *v = nav.active())
        v->screen().markAllDirty(); /* repaint fully on wake */
    return true;
}

} // namespace

/* Pure idle-timeout predicate — C linkage so the unit test (ui_check_standby)
 * links against it exactly as it did the classic implementation. Mirrors the
 * classic thresholds verbatim. */
extern "C" bool _ui_checkStandby(long long time_since_last_event)
{
    if (standby)
        return false;

    switch (state.settings.display_timer) {
        case TIMER_OFF:
            return false;
        case TIMER_5S:
        case TIMER_10S:
        case TIMER_15S:
        case TIMER_20S:
        case TIMER_25S:
        case TIMER_30S:
            return time_since_last_event >= (5000 * state.settings.display_timer);
        case TIMER_1M:
        case TIMER_2M:
        case TIMER_3M:
        case TIMER_4M:
        case TIMER_5M:
            return time_since_last_event >=
                (60000 * (state.settings.display_timer - (TIMER_1M - 1)));
        case TIMER_15M:
        case TIMER_30M:
        case TIMER_45M:
            return time_since_last_event >=
                (60000 * 15 * (state.settings.display_timer - (TIMER_15M - 1)));
        case TIMER_1H:
            return time_since_last_event >= 60 * 60 * 1000;
    }

    return false;
}

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
    fmView.build();
    radioView.build();
    defaultsView.build();
    channelsView.build();
    contactsView.build();
    banksView.build();
#ifdef CONFIG_M17
    m17View.build();
#endif
#ifdef CONFIG_GPS
    gpsView.build();
    gpsSettingsView.build();
#endif

    /* Wire menu rows to their views. Rows resolve by label because the indices
     * shift with the config guards on the menu tables. */
    wireRow(mainMenu, kMainMenu, kMainMenuCount, "Banks", &banksView);
    wireRow(mainMenu, kMainMenu, kMainMenuCount, "Channels", &channelsView);
    wireRow(mainMenu, kMainMenu, kMainMenuCount, "Contacts", &contactsView);
    wireRow(mainMenu, kMainMenu, kMainMenuCount, "Settings", &settingsMenu);
    wireRow(mainMenu, kMainMenu, kMainMenuCount, "Info", &infoView);
    wireRow(mainMenu, kMainMenu, kMainMenuCount, "About", &aboutView);
    wireRow(settingsMenu, kSettingsMenu, kSettingsMenuCount, "Display",
            &displayView);
    wireRow(settingsMenu, kSettingsMenu, kSettingsMenuCount, "Accessibility",
            &checklistView);
    wireRow(settingsMenu, kSettingsMenu, kSettingsMenuCount, "FM", &fmView);
    wireRow(settingsMenu, kSettingsMenu, kSettingsMenuCount, "Radio", &radioView);
    wireRow(settingsMenu, kSettingsMenu, kSettingsMenuCount, "Default Settings",
            &defaultsView);
#ifdef CONFIG_M17
    wireRow(settingsMenu, kSettingsMenu, kSettingsMenuCount, "M17", &m17View);
#endif
#ifdef CONFIG_GPS
    wireRow(mainMenu, kMainMenu, kMainMenuCount, "GPS", &gpsView);
    wireRow(settingsMenu, kSettingsMenu, kSettingsMenuCount, "GPS",
            &gpsSettingsView);
#endif

    vfoView.setMenu(&mainMenu);
    nav.setRoot(&vfoView);

    standby = false;
    last_event_tick = getTick();

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
    const long long now = getTick();

    if (evQueue_rdPos != evQueue_wrPos) {
        /* Pop one event per tick, matching the classic loop cadence. */
        const event_t raw = evQueue[evQueue_rdPos];
        evQueue_rdPos = (uint8_t)((evQueue_rdPos + 1) % MAX_NUM_EVENTS);

        /* A keypress always refreshes the idle timer; if it also woke the
         * display we consume it (except MONI, kept for the macro menu) so the
         * wake press isn't acted on — parity with the classic UI. */
        if (raw.type == EVENT_KBD) {
            kbd_msg_t msg;
            msg.value = raw.payload;
            const bool woke = exitStandby(now);
            if (woke && ((msg.keys & KEY_MONI) == 0u))
                return;
        }

        /* The view that handles the event is the active one at dispatch time; if
         * it edited an rtx-affecting field it flags a resync, which we forward so
         * threads.c re-applies state.channel to the radio. */
        View *handler = nav.active();
        const Event ev = Event::decode((uint8_t)raw.type, raw.payload);
        nav.dispatch(ev);

        if ((handler != nullptr) && handler->takeSyncRtx())
            *sync_rtx = true;
        return;
    }

    /* No event this tick: ongoing RF or a volume change keeps the screen awake,
     * otherwise blank the backlight once the idle timer elapses. */
    const bool txOngoing = (rtx_getStatus()->opStatus == TX);
    if (txOngoing || rtx_rxSquelchOpen() || (state.volume != last_state.volume)) {
        exitStandby(now);
        return;
    }
    if (_ui_checkStandby(now - last_event_tick))
        enterStandby();
}

extern "C" bool ui_updateGUI()
{
    if (standby)
        return false; /* backlight off — nothing to draw */

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
