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
        case Sem::Background:
        case Sem::Surface:
        case Sem::SurfaceHigh:
            return palette::black;
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
        case Sem::OnSurface:
            return palette::onSurface;
        case Sem::OnSurfaceMuted:
            return palette::onSurfaceMut;
        case Sem::Primary:
        case Sem::FocusRing:
            return palette::brandGold;
        case Sem::OnPrimary:
            return palette::background;
        case Sem::TxDanger:
            return palette::txRed;
        case Sem::RxSuccess:
        case Sem::ModeDMR:
            return palette::rxGreen;
        case Sem::Warning:
            return palette::warnOrange;
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
