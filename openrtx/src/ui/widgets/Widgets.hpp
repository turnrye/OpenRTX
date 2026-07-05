/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_WIDGETS_HPP
#define ORTX_UI_WIDGETS_HPP

#include "core/Object.hpp"
#include "core/graphics.h"
#include "style/SemanticColor.hpp"

namespace ortxui
{

/**
 * A single-line text label drawn within its layout-assigned area. The text is
 * measured and placed relative to the box (horizontally per `align`, vertically
 * centred), so a layout container positions the label just by sizing its area.
 */
class Label : public Object
{
public:
    void setText(const char *t)
    {
        text_ = (t != nullptr) ? t : "";
    }
    void setColor(Sem c)
    {
        color_ = c;
    }
    void setFont(fontSize_t f)
    {
        font_ = f;
    }
    void setAlign(textAlign_t a)
    {
        align_ = a;
    }

    Size natural() const override; //< measured text width x font height
    void draw(DrawCtx &d) override;

private:
    const char *text_ = "";
    Sem color_ = Sem::OnSurface;
    fontSize_t font_ = FONT_SIZE_8PT;
    textAlign_t align_ = TEXT_ALIGN_LEFT;
};

/**
 * A solid filled rectangle in a role colour (status/action bars, surfaces).
 */
class Panel : public Object
{
public:
    void setColor(Sem c)
    {
        color_ = c;
    }

    void draw(DrawCtx &d) override;

private:
    Sem color_ = Sem::Surface;
};

/**
 * A horizontal level/progress bar. `value` is clamped to [0, 1].
 */
class Bar : public Object
{
public:
    void setValue(float v)
    {
        value_ = v;
    }
    void setColors(Sem track, Sem fill)
    {
        track_ = track;
        fill_ = fill;
    }

    void draw(DrawCtx &d) override;

private:
    float value_ = 0.0f;
    Sem track_ = Sem::SurfaceHigh;
    Sem fill_ = Sem::RxSuccess;
};

/**
 * A filled chip with a centred label, e.g. a protocol mode badge. Intended for
 * screen-centred placement (alignment resolves against the screen width).
 */
class Chip : public Object
{
public:
    void setText(const char *t)
    {
        text_ = (t != nullptr) ? t : "";
    }
    void setColors(Sem fill, Sem textColor)
    {
        fill_ = fill;
        textColor_ = textColor;
    }

    void draw(DrawCtx &d) override;

private:
    const char *text_ = "";
    Sem fill_ = Sem::Surface;
    Sem textColor_ = Sem::OnPrimary;
};

} // namespace ortxui

#endif /* ORTX_UI_WIDGETS_HPP */
