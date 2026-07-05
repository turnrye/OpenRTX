/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_SEMANTICCOLOR_HPP
#define ORTX_UI_SEMANTICCOLOR_HPP

#include <cstdint>

namespace ortxui
{

/**
 * Semantic colour roles.
 *
 * Widgets reference roles, never raw colours. A Theme maps each role to a
 * concrete framebuffer colour for the active target, and collapses them to
 * ink/paper on 1bpp monochrome targets. This is what lets one screen render
 * in full colour on the 160x128 RGB565 panels and legibly in black & white on
 * the 128x64 mono panels with no per-widget branching.
 *
 * See the "Instrument" visual design language in
 * /home/turnrye/.claude/plans/precious-yawning-lighthouse.md
 */
enum class Sem : uint8_t {
    Background = 0, //< Base app background (near-black charcoal)
    Surface,        //< Card / bar surface, one step above background
    SurfaceHigh,    //< Selected row / raised surface
    OnSurface,      //< Primary foreground text (warm off-white)
    OnSurfaceMuted, //< Secondary / disabled text (grey)
    Primary,        //< OpenRTX brand accent (gold): focus ring, headers
    OnPrimary,      //< Text/icon drawn on top of a Primary fill
    TxDanger,       //< Transmit / danger (red): chrome flips to this on TX
    RxSuccess,      //< Receive / squelch-open (green)
    Warning,        //< Battery-low / caution (orange)
    ModeM17,        //< M17 protocol brand red
    ModeM17Accent,  //< M17 logo secondary grey
    ModeFM,         //< FM mode badge (muted cyan-grey)
    ModeDMR,        //< DMR mode badge (reuses RxSuccess green)
    FocusRing,      //< Focus highlight (reuses Primary gold)

    Count
};

} // namespace ortxui

#endif /* ORTX_UI_SEMANTICCOLOR_HPP */
