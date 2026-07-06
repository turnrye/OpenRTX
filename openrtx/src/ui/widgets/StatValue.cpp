/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "widgets/StatValue.hpp"
#include "render/DrawCtx.hpp"

#include <cstring>

namespace ortxui
{

void StatValue::setValue(const char *v)
{
    strncpy(value_, (v != nullptr) ? v : "", sizeof(value_) - 1);
    value_[sizeof(value_) - 1] = '\0';
}

void StatValue::setUnit(const char *u)
{
    strncpy(unit_, (u != nullptr) ? u : "", sizeof(unit_) - 1);
    unit_[sizeof(unit_) - 1] = '\0';
}

void StatValue::draw(DrawCtx &d)
{
    /* The unit label hugs the right edge; the value fills the space to its
     * left, right-aligned so the values form a clean column. */
    const int16_t gap = 3;
    const uint16_t unitW =
        (unit_[0] != '\0') ? static_cast<uint16_t>(
                                 gfx_getTextWidth(FONT_SIZE_6PT, unit_) + gap) :
                             0u;

    const Rect unitBox = { static_cast<int16_t>(area_.right() - unitW + gap
                                                + 1),
                           area_.y, unitW, area_.h };
    if (unit_[0] != '\0')
        d.textInBox(unitBox, FONT_SIZE_6PT, TEXT_ALIGN_RIGHT, unitColor_,
                    unit_);

    const Rect valBox = { area_.x, area_.y,
                          static_cast<uint16_t>(area_.w - unitW), area_.h };
    d.textInBox(valBox, valueFont_, TEXT_ALIGN_RIGHT, valueColor_, value_);
}

} // namespace ortxui
