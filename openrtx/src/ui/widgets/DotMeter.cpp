/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "widgets/DotMeter.hpp"
#include "render/DrawCtx.hpp"

#include <cstdio>

namespace ortxui
{

namespace
{
constexpr int kDotR = 3;  //< dot radius
constexpr int kPitch = 9; //< dot centre-to-centre spacing
constexpr int kGap = 5;   //< gap around the label / readout
const fontSize_t kFont = FONT_SIZE_6PT;

bool isScalePoint(uint8_t sPoint)
{
    return (sPoint == 3) || (sPoint == 5) || (sPoint == 7) || (sPoint == 9);
}
} // namespace

Size DotMeter::natural() const
{
    return { 200, static_cast<uint16_t>(2 * kDotR + 4) };
}

void DotMeter::draw(DrawCtx &d)
{
    const int16_t cy = static_cast<int16_t>(area_.y + area_.h / 2);
    int16_t x = static_cast<int16_t>(area_.x + 2);

    if (label_[0] != '\0') {
        const uint16_t lw = gfx_getTextWidth(kFont, label_);
        const Rect box = { x, area_.y, lw, area_.h };
        d.textInBox(box, kFont, TEXT_ALIGN_LEFT, color_, label_);
        x = static_cast<int16_t>(x + lw + kGap);
    }

    for (uint8_t i = 0; i < total_; i++) {
        const int16_t cx = static_cast<int16_t>(x + kDotR + i * kPitch);
        const uint8_t sPoint = static_cast<uint8_t>(i + 1);
        const bool lit = (i < filled_);
        const Sem tone = lit ? color_ : Sem::OnSurfaceMuted;

        if (showScale_ && isScalePoint(sPoint)) {
            char num[4];
            snprintf(num, sizeof(num), "%u", sPoint);
            const Rect box = { static_cast<int16_t>(cx - kDotR), area_.y,
                               static_cast<uint16_t>(2 * kDotR + 2), area_.h };
            d.textInBox(box, kFont, TEXT_ALIGN_CENTER, tone, num);
        } else if (lit) {
            d.fillCircle({ cx, cy }, kDotR, color_);
        } else {
            d.drawCircle({ cx, cy }, kDotR, Sem::OnSurfaceMuted);
        }
    }

    x = static_cast<int16_t>(x + total_ * kPitch + kGap);

    if (readout_[0] != '\0') {
        const uint16_t rw = gfx_getTextWidth(kFont, readout_);
        const Rect box = { x, area_.y, rw, area_.h };
        d.textInBox(box, kFont, TEXT_ALIGN_LEFT, color_, readout_);
    }
}

} // namespace ortxui
