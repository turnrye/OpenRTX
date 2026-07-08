/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "widgets/DotMeter.hpp"
#include "render/DrawCtx.hpp"

#include <cmath>
#include <cstdio>

namespace ortxui
{

namespace
{
constexpr int kDotR = 3; //< dot radius
constexpr int kGap = 5;  //< gap around the label / readout
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
    const int16_t pad = 3; //< outer gutter
    const int16_t cy = static_cast<int16_t>(area_.y + area_.h / 2);
    int16_t left = static_cast<int16_t>(area_.x + pad);
    int16_t right = static_cast<int16_t>(area_.x + area_.w - pad);

    /* Label pinned to the left. */
    if (label_[0] != '\0') {
        const uint16_t lw = gfx_getTextWidth(kFont, label_);
        d.textInBox({ left, area_.y, lw, area_.h }, kFont, TEXT_ALIGN_LEFT,
                    color_, label_);
        left = static_cast<int16_t>(left + lw + kGap);
    }

    /* Readout pinned to the right. */
    if (readout_[0] != '\0') {
        const uint16_t rw = gfx_getTextWidth(kFont, readout_);
        d.textInBox({ static_cast<int16_t>(right - rw), area_.y, rw, area_.h },
                    kFont, TEXT_ALIGN_LEFT, color_, readout_);
        right = static_cast<int16_t>(right - rw - kGap);
    }

    /* Dots spread evenly across the space between the label and the readout,
     * so the meter spans the full width of the screen. */
    if ((total_ == 0u) || (right <= left))
        return;
    const float cell = static_cast<float>(right - left) / total_;

    for (uint8_t i = 0; i < total_; i++) {
        const int16_t cx =
            static_cast<int16_t>(left + std::lround((i + 0.5f) * cell));
        const uint8_t sPoint = static_cast<uint8_t>(i + 1);
        const bool lit = (i < filled_);
        const Sem tone = lit ? color_ : Sem::OnSurfaceMuted;

        if (showScale_ && isScalePoint(sPoint)) {
            char num[4];
            snprintf(num, sizeof(num), "%u", sPoint);
            const Rect box = { static_cast<int16_t>(cx - kDotR - 1), area_.y,
                               static_cast<uint16_t>(2 * kDotR + 3), area_.h };
            d.textInBox(box, kFont, TEXT_ALIGN_CENTER, tone, num);
        } else if (lit) {
            d.fillCircle({ cx, cy }, kDotR, color_);
        } else {
            d.drawCircle({ cx, cy }, kDotR, Sem::OnSurfaceMuted);
        }
    }
}

} // namespace ortxui
