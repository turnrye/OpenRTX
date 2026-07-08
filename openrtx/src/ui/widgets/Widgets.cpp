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
    /* Reserve the full line height (ascent + descent), not just the ascent, so
     * a layout that centres the label leaves room for descenders below the
     * baseline instead of clipping them at the frame edge. */
    return { gfx_getTextWidth(font_, text_), gfx_getFontLineHeight(font_) };
}

void Label::draw(DrawCtx &d)
{
    if (overflow_ == Overflow::Ellipsize)
        d.textInBoxEllipsized(area_, font_, align_, color_, text_);
    else
        d.textInBox(area_, font_, align_, color_, text_);
}

Size TaggedValue::natural() const
{
    uint16_t w = gfx_getTextWidth(valFont_, value_);
    if (tag_[0] != '\0')
        w = static_cast<uint16_t>(w + gfx_getTextWidth(tagFont_, tag_) + 3);
    return { w, gfx_getFontLineHeight(valFont_) };
}

void TaggedValue::draw(DrawCtx &d)
{
    if (area_.empty())
        return;

    const uint16_t valW = gfx_getTextWidth(valFont_, value_);
    const bool hasTag = (tag_[0] != '\0');
    const uint16_t tagW = hasTag ? gfx_getTextWidth(tagFont_, tag_) : 0;
    const int16_t gap = hasTag ? 3 : 0;

    /* Bottom-align the (smaller) tag on the value's baseline, and centre the
     * value line vertically in the box. */
    const int16_t valAsc = static_cast<int16_t>(gfx_getFontAscent(valFont_));
    const int16_t valLH = static_cast<int16_t>(gfx_getFontLineHeight(valFont_));
    const int16_t baseline =
        static_cast<int16_t>(area_.y + (area_.h - valLH) / 2 + valAsc);

    const int16_t valX = static_cast<int16_t>(area_.right() - valW + 1);
    d.text({ valX, baseline }, valFont_, TEXT_ALIGN_LEFT, color_, value_);
    if (hasTag) {
        const int16_t tagX = static_cast<int16_t>(valX - gap - tagW);
        d.text({ tagX, baseline }, tagFont_, TEXT_ALIGN_LEFT, color_, tag_);
    }
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

} // namespace ortxui
