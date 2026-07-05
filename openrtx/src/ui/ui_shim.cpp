/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * ui_shim.cpp -- the C ABI boundary of the "ortx" UI toolkit.
 *
 * This file implements the eight functions of core/ui.h (the frozen seam the
 * rest of the firmware calls, only from threads.c / openrtx.c / state.c) and
 * forwards into the C++ retained-widget toolkit. threads.c, graphics.c, the
 * state module, the event queue and the sync_rtx/mutex discipline are all
 * unchanged.
 *
 * At this scaffolding stage the toolkit renders a single themed "Instrument"
 * demo screen so the seam, the theme system and the render backend can be
 * exercised end to end in the linux emulator. The retained Object/Screen/Event
 * core and the per-screen views land next.
 */

#include "core/ui.h"
#include "core/state.h"
#include "core/event.h"
#include "core/graphics.h"
#include "interfaces/keyboard.h"
#include "rtx/rtx.h"
#include "hwconfig.h"

#include "render/DrawCtx.hpp"
#include "style/SemanticColor.hpp"

#include <cstdint>
#include <cstdio>

using namespace ortxui;

namespace
{

/* Mirror of the classic UI event ring buffer: single-thread producer/consumer
 * (all inside the UI thread), so no locking is required. */
constexpr uint8_t MAX_NUM_EVENTS = 16;

event_t evQueue[MAX_NUM_EVENTS];
uint8_t evQueue_rdPos = 0;
uint8_t evQueue_wrPos = 0;

/* UI-thread-local snapshot of the radio state. ui_saveState() copies the
 * global `state` here under state_mutex; all drawing reads this snapshot,
 * preserving the classic decouple-render-from-mutation pattern. */
state_t last_state;

bool redraw_needed = true;

/* Resolve a channel operating mode to its badge colour role and label. */
Sem modeBadge(uint8_t mode, const char *&label)
{
    switch (mode) {
        case OPMODE_FM:
            label = "FM";
            return Sem::ModeFM;
        case OPMODE_DMR:
            label = "DMR";
            return Sem::ModeDMR;
        case OPMODE_M17:
            label = "M17";
            return Sem::ModeM17;
        default:
            label = "--";
            return Sem::OnSurfaceMuted;
    }
}

/* Render the themed "Instrument" home screen from the state snapshot. */
void drawInstrumentDemo()
{
    DrawCtx d;
    d.clearScreen(Sem::Background);

    const uint16_t W = CONFIG_SCREEN_WIDTH;
    const uint16_t H = CONFIG_SCREEN_HEIGHT;
    const uint16_t topH = 16;
    const uint16_t botH = 16;

    char buf[24];

    /* ---- Top status bar ---- */
    const Rect topbar = { 0, 0, W, topH };
    d.fillRect(topbar, Sem::Surface);

    /* Text baseline sits ~11px down so 6pt glyphs clear the top edge. */
    const int16_t topTextY = 11;

    snprintf(buf, sizeof(buf), "%02u:%02u", last_state.time.hour,
             last_state.time.minute);
    d.text(Point{ 4, topTextY }, FONT_SIZE_6PT, TEXT_ALIGN_LEFT, Sem::OnSurface,
           buf);

    snprintf(buf, sizeof(buf), "%u%%", last_state.charge);
    d.text(Point{ (int16_t)(W - 4), topTextY }, FONT_SIZE_6PT, TEXT_ALIGN_RIGHT,
           Sem::OnSurface, buf);

    /* Mode badge chip, centred in the status bar. */
    const char *modeLabel = "--";
    const Sem modeSem = modeBadge(last_state.channel.mode, modeLabel);
    const Rect chip = { (int16_t)((W / 2) - 16), 2, 32, 12 };
    d.fillRect(chip, modeSem);
    d.text(Point{ (int16_t)(W / 2), topTextY }, FONT_SIZE_6PT,
           TEXT_ALIGN_CENTER, Sem::OnPrimary, modeLabel);

    /* ---- Frequency hero ---- */
    const unsigned long f = (unsigned long)last_state.channel.rx_frequency;
    snprintf(buf, sizeof(buf), "%lu.%05lu", f / 1000000UL,
             (f % 1000000UL) / 10UL);
    d.text(Point{ (int16_t)(W / 2), (int16_t)(topH + 22) }, FONT_SIZE_16PT,
           TEXT_ALIGN_CENTER, Sem::OnSurface, buf);

    /* ---- Sub line: operator callsign ---- */
    d.text(Point{ (int16_t)(W / 2), (int16_t)(topH + 40) }, FONT_SIZE_8PT,
           TEXT_ALIGN_CENTER, Sem::OnSurfaceMuted,
           last_state.settings.callsign);

    /* ---- S-meter demo bar ---- */
    const int16_t barY = (int16_t)(H - botH - 14);
    const uint16_t barW = (uint16_t)(W - 8);
    const Rect barBg = { 4, barY, barW, 8 };
    d.fillRect(barBg, Sem::SurfaceHigh);
    const Rect barFill = { 4, barY, (uint16_t)(barW / 2), 8 };
    d.fillRect(barFill, Sem::RxSuccess);

    /* ---- Bottom action bar ---- */
    const Rect botbar = { 0, (int16_t)(H - botH), W, botH };
    d.fillRect(botbar, Sem::Surface);
    d.text(Point{ 4, (int16_t)(H - botH + 3) }, FONT_SIZE_6PT, TEXT_ALIGN_LEFT,
           Sem::Primary, "VFO");
    d.text(Point{ (int16_t)(W - 4), (int16_t)(H - botH + 3) }, FONT_SIZE_6PT,
           TEXT_ALIGN_RIGHT, Sem::Primary, "TONE");
}

} // namespace

extern "C" void ui_init()
{
    evQueue_rdPos = 0;
    evQueue_wrPos = 0;
    redraw_needed = true;
    last_state = state;
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
    const uint8_t next = (uint8_t)((evQueue_rdPos + 1) % MAX_NUM_EVENTS);
    evQueue_rdPos = next;
    redraw_needed = true;
}

extern "C" bool ui_updateGUI()
{
    if (!redraw_needed)
        return false;

    drawInstrumentDemo();
    redraw_needed = false;
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
