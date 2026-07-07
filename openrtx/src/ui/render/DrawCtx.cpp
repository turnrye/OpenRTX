/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "render/DrawCtx.hpp"
#include "style/Theme.hpp"
#include "hwconfig.h"

#include <cmath>

namespace ortxui
{

DrawCtx::DrawCtx() : clip_{ 0, 0, CONFIG_SCREEN_WIDTH, CONFIG_SCREEN_HEIGHT }
{
}

void DrawCtx::clearScreen(Sem bg)
{
    gfx_fillScreen(themeColor(bg));
}

void DrawCtx::fillRect(const Rect &r, Sem color)
{
    if (r.empty())
        return;

    const point_t start = { r.x, r.y };
    gfx_drawRect(start, r.w, r.h, themeColor(color), true);
}

void DrawCtx::drawRect(const Rect &r, Sem color)
{
    if (r.empty())
        return;

    const point_t start = { r.x, r.y };
    gfx_drawRect(start, r.w, r.h, themeColor(color), false);
}

void DrawCtx::drawCircle(Point c, uint16_t r, Sem color)
{
    /* Anti-aliased ~1px ring: coverage peaks on the radius and fades either
     * side; gfx_setPixel blends the modulated alpha (thresholded on mono). */
    const color_t col = themeColor(color);
    const float rf = static_cast<float>(r);
    const int R = static_cast<int>(r) + 1;
    for (int dy = -R; dy <= R; dy++) {
        for (int dx = -R; dx <= R; dx++) {
            const float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy));
            float cov = 1.0f - std::fabs(dist - rf);
            if (cov <= 0.0f)
                continue;
            if (cov > 1.0f)
                cov = 1.0f;
            color_t p = col;
            p.alpha = static_cast<uint8_t>(col.alpha * cov);
            gfx_setPixel({ static_cast<int16_t>(c.x + dx),
                           static_cast<int16_t>(c.y + dy) },
                         p);
        }
    }
}

void DrawCtx::fillCircle(Point c, uint16_t r, Sem color)
{
    /* Anti-aliased disc: full coverage inside with a 1px soft edge. */
    const color_t col = themeColor(color);
    const float rf = static_cast<float>(r);
    const int R = static_cast<int>(r) + 1;
    for (int dy = -R; dy <= R; dy++) {
        for (int dx = -R; dx <= R; dx++) {
            const float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy));
            float cov = rf + 0.5f - dist;
            if (cov <= 0.0f)
                continue;
            if (cov > 1.0f)
                cov = 1.0f;
            color_t p = col;
            p.alpha = static_cast<uint8_t>(col.alpha * cov);
            gfx_setPixel({ static_cast<int16_t>(c.x + dx),
                           static_cast<int16_t>(c.y + dy) },
                         p);
        }
    }
}

void DrawCtx::line(Point a, Point b, Sem color)
{
    const point_t start = { a.x, a.y };
    const point_t end = { b.x, b.y };
    gfx_drawLine(start, end, themeColor(color));
}

point_t DrawCtx::text(Point at, fontSize_t size, textAlign_t align, Sem color,
                      const char *str)
{
    const point_t start = { at.x, at.y };
    /* "%s" keeps arbitrary content (including '%') safe through the backend's
     * printf-style formatter. */
    return gfx_print(start, size, align, themeColor(color), "%s", str);
}

void DrawCtx::textInBox(const Rect &box, fontSize_t size, textAlign_t align,
                        Sem color, const char *str)
{
    const int tw = static_cast<int>(gfx_getTextWidth(size, str));
    const int fh = static_cast<int>(gfx_getFontHeight(size));

    int16_t x = box.x;
    if (align == TEXT_ALIGN_CENTER)
        x = static_cast<int16_t>(box.x + (static_cast<int>(box.w) - tw) / 2);
    else if (align == TEXT_ALIGN_RIGHT)
        x = static_cast<int16_t>(box.x + static_cast<int>(box.w) - tw);

    /* The backend anchors a line by its baseline; centre it in the box. */
    const int16_t baseline =
        static_cast<int16_t>(box.y + (static_cast<int>(box.h) + fh) / 2 - 1);
    const point_t start = { x, baseline };
    gfx_print(start, size, TEXT_ALIGN_LEFT, themeColor(color), "%s", str);
}

} // namespace ortxui
