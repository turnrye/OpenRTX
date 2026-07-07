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

    /** Narrow the clip to the intersection of the current clip and `r`, and
     *  push the previous clip so popClip() restores it. Drawing outside the
     *  active clip is suppressed by the backend. */
    void pushClip(const Rect &r);

    /** Restore the clip saved by the matching pushClip(). */
    void popClip();

    /** Fill the whole screen with a role colour. */
    void clearScreen(Sem bg);

    /** Filled rectangle. */
    void fillRect(const Rect &r, Sem color);

    /** 1px rectangle outline. */
    void drawRect(const Rect &r, Sem color);

    /** 1px circle outline centred at `c`. */
    void drawCircle(Point c, uint16_t r, Sem color);

    /** Filled disc centred at `c` (scanline fill; no backend primitive). */
    void fillCircle(Point c, uint16_t r, Sem color);

    /** 1px line between two points. */
    void line(Point a, Point b, Sem color);

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

    /**
     * Like textInBox(), but if the text is wider than the box it is truncated
     * and an ellipsis ("...") is appended so it fits. Used for list values and
     * labels that may hold arbitrary-length content.
     */
    void textInBoxEllipsized(const Rect &box, fontSize_t size,
                             textAlign_t align, Sem color, const char *str);

private:
    static constexpr uint8_t kClipStackMax = 16;
    Rect clip_;
    Rect clipStack_[kClipStackMax];
    uint8_t clipDepth_ = 0;
};

} // namespace ortxui

#endif /* ORTX_UI_DRAWCTX_HPP */
