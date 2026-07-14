/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_THEME_HPP
#define ORTX_UI_THEME_HPP

#include "core/graphics.h"
#include "style/SemanticColor.hpp"

namespace ortxui
{

/**
 * Resolve a semantic colour role to a concrete framebuffer colour for the
 * active build target.
 *
 * On RGB565 colour targets this returns the "Instrument" palette entry for the
 * role, or the monochrome ink/paper resolution when the user selects the High
 * Contrast theme (settings.highContrast). On 1bpp monochrome targets
 * (CONFIG_PIX_FMT_BW) roles always collapse to ink/paper, so the theme setting
 * is a no-op there. All call sites go through this one function, so a theme
 * change takes effect on the next repaint with no other plumbing.
 */
color_t themeColor(Sem role);

} // namespace ortxui

#endif /* ORTX_UI_THEME_HPP */
