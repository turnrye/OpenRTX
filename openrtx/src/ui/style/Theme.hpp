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
 * role. On 1bpp monochrome targets (CONFIG_PIX_FMT_BW) roles collapse to
 * ink/paper so the UI stays fully legible with zero colour. There is a single
 * active theme; a runtime theme object can be introduced later without
 * changing call sites, which all go through this function.
 */
color_t themeColor(Sem role);

} // namespace ortxui

#endif /* ORTX_UI_THEME_HPP */
