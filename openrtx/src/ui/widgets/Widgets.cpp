/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "widgets/Widgets.hpp"
#include "render/DrawCtx.hpp"

namespace ortxui
{

Size Label::natural() const
{
    return { gfx_getTextWidth(font_, text_), gfx_getFontHeight(font_) };
}

void Label::draw(DrawCtx &d)
{
    d.textInBox(area_, font_, align_, color_, text_);
}

void Panel::draw(DrawCtx &d)
{
    d.fillRect(area_, color_);
}

void Bar::draw(DrawCtx &d)
{
    d.fillRect(area_, track_);

    float v = value_;
    if (v < 0.0f)
        v = 0.0f;
    if (v > 1.0f)
        v = 1.0f;

    const uint16_t fillW = static_cast<uint16_t>(area_.w * v);
    if (fillW > 0) {
        const Rect fill = { area_.x, area_.y, fillW, area_.h };
        d.fillRect(fill, fill_);
    }
}

void Chip::draw(DrawCtx &d)
{
    d.fillRect(area_, fill_);

    const int16_t fh = static_cast<int16_t>(gfx_getFontHeight(FONT_SIZE_6PT));
    const int16_t y = static_cast<int16_t>(area_.y + (area_.h + fh) / 2 - 1);
    const Point at = { static_cast<int16_t>(area_.x + area_.w / 2), y };
    d.text(at, FONT_SIZE_6PT, TEXT_ALIGN_CENTER, textColor_, text_);
}

} // namespace ortxui
