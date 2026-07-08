/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "style/Theme.hpp"
#include "style/Palette.hpp"
#include "hwconfig.h"

namespace ortxui
{

#if defined(CONFIG_PIX_FMT_BW)

/*
 * Monochrome resolution. Structure and type carry the UI; colour collapses to
 * ink/paper. Background-like roles become the cleared (paper) state, every
 * foreground role becomes ink.
 */
color_t themeColor(Sem role)
{
    switch (role) {
        /* Paper (cleared) roles and text-on-fill collapse to black. */
        case Sem::Background:
        case Sem::Surface:
        case Sem::SurfaceHigh:
        case Sem::Separator:
        case Sem::OnPrimary:
        case Sem::OnAccent:
            return palette::black;
        /* Everything foreground becomes ink; selection reads as an inverted
         * (white) fill with black text. */
        default:
            return palette::white;
    }
}

#else

/*
 * Colour resolution: the "Instrument" palette.
 */
color_t themeColor(Sem role)
{
    switch (role) {
        case Sem::Background:
            return palette::background;
        case Sem::Surface:
            return palette::surface;
        case Sem::SurfaceHigh:
            return palette::surfaceHigh;
        case Sem::Separator:
            return palette::separator;
        case Sem::OnSurface:
            return palette::onSurface;
        case Sem::OnSurfaceMuted:
            return palette::onSurfaceMut;
        case Sem::Primary:
            return palette::selectionBlue;
        case Sem::OnPrimary:
        case Sem::OnAccent:
            return palette::black;
        case Sem::Accent:
        case Sem::FocusRing:
            return palette::brandGold;
        case Sem::TxDanger:
            return palette::txOrange;
        case Sem::RxSuccess:
        case Sem::ModeDMR:
            return palette::rxGreen;
        case Sem::Warning:
            return palette::warnOrange;
        case Sem::Mark:
            return palette::markRed;
        case Sem::ModeM17:
            return palette::m17Red;
        case Sem::ModeM17Accent:
            return palette::m17Grey;
        case Sem::ModeFM:
            return palette::fmCyanGrey;
        default:
            return palette::onSurface;
    }
}

#endif

} // namespace ortxui
