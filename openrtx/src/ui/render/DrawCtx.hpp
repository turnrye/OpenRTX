/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_DRAWCTX_HPP
#define ORTX_UI_DRAWCTX_HPP

#include "core/graphics.h"
#include "core/Geometry.hpp"
#include "style/SemanticColor.hpp"

namespace ortxui
{

/**
 * Thin, theme-aware drawing surface layered over the gfx_* backend.
 *
 * Every colour is supplied as a semantic role and resolved through the active
 * Theme, so drawing code never mentions raw colours. The context also carries
 * a clip rectangle; today drawing is full-screen, but the clip is threaded
 * through so dirty-rectangle rendering can be enabled as the widget tree
 * lands, without touching call sites.
 *
 * The gfx_* backend owns the static framebuffer; the toolkit never allocates
 * one. Flushing to the panel stays the responsibility of the UI loop
 * (threads.c calls gfx_render after ui_updateGUI reports a redraw).
 */
class DrawCtx
{
public:
    DrawCtx();

    /** Current clip rectangle (screen space). */
    const Rect &clip() const
    {
        return clip_;
    }

    /** Fill the whole screen with a role colour. */
    void clearScreen(Sem bg);

    /** Filled rectangle. */
    void fillRect(const Rect &r, Sem color);

    /** 1px rectangle outline. */
    void drawRect(const Rect &r, Sem color);

    /**
     * Draw a NUL-terminated string. Returns the drawn text extent as reported
     * by the backend. Multi-line strings (with '\n') are supported.
     */
    point_t text(Point at, fontSize_t size, textAlign_t align, Sem color,
                 const char *str);

    /**
     * Draw a single line of text aligned within a box. Unlike the raw backend,
     * whose CENTER/RIGHT resolve against the whole screen, this measures the
     * text and places it relative to `box`: horizontally per `align`, and
     * vertically centred. This is what lets layout-assigned areas drive text.
     */
    void textInBox(const Rect &box, fontSize_t size, textAlign_t align,
                   Sem color, const char *str);

private:
    Rect clip_;
};

} // namespace ortxui

#endif /* ORTX_UI_DRAWCTX_HPP */
