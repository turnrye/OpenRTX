/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "widgets/FreqHero.hpp"
#include "render/DrawCtx.hpp"
#include "core/graphics.h"

#include <cstdio>

namespace ortxui
{

void FreqHero::setFreq(uint32_t hz)
{
    const uint32_t mhz = hz / 1000000UL;
    const uint32_t khz = (hz % 1000000UL) / 1000UL; //< 3-digit kHz
    const uint32_t sub = (hz % 1000UL) / 10UL;      //< 2-digit, 10 Hz res

    snprintf(mainBuf_, sizeof(mainBuf_), "%lu.%03lu", (unsigned long)mhz,
             (unsigned long)khz);
    snprintf(subBuf_, sizeof(subBuf_), "%02lu", (unsigned long)sub);
}

void FreqHero::draw(DrawCtx &d)
{
    /* Narrow screens (128px Compact: gd77/dm1801) cannot fit the 16pt frequency
     * next to a 40px mode stack, so shrink both on a width threshold. */
    const bool compact = (area_.w < 150);
    const fontSize_t mainFont = compact ? FONT_SIZE_12PT : FONT_SIZE_16PT;
    const int16_t modeW = compact ? 28 : 40;

    /* Frequency + sub-digits share a baseline near the bottom of the band. */
    const int16_t indent = 4;
    const int16_t baseY = static_cast<int16_t>(area_.bottom() - 3);

    const uint16_t mainW = gfx_getTextWidth(mainFont, mainBuf_);
    const Point mainAt = { static_cast<int16_t>(area_.x + indent), baseY };
    d.text(mainAt, mainFont, TEXT_ALIGN_LEFT, Sem::OnSurface, mainBuf_);

    /* Only draw the trailing sub-kHz digits if they clear the mode column. */
    const int16_t modeLeft = static_cast<int16_t>(area_.right() - modeW);
    const int16_t subX = static_cast<int16_t>(area_.x + indent + mainW + 2);
    const uint16_t subW = gfx_getTextWidth(FONT_SIZE_8PT, subBuf_);
    if (subX + subW <= modeLeft - 2) {
        const Point subAt = { subX, baseY };
        d.text(subAt, FONT_SIZE_8PT, TEXT_ALIGN_LEFT, Sem::OnSurface, subBuf_);
    }

    /* Right-aligned mode stack: mode/bw (top), tone (mid), power (bottom). */
    const int16_t mx = modeLeft;
    const int16_t lh = static_cast<int16_t>(area_.h / 3);
    const Rect r1 = { mx, area_.y, static_cast<uint16_t>(modeW),
                      static_cast<uint16_t>(lh) };
    const Rect r2 = { mx, static_cast<int16_t>(area_.y + lh),
                      static_cast<uint16_t>(modeW), static_cast<uint16_t>(lh) };
    const Rect r3 = { mx, static_cast<int16_t>(area_.y + 2 * lh),
                      static_cast<uint16_t>(modeW), static_cast<uint16_t>(lh) };
    d.textInBox(r1, FONT_SIZE_8PT, TEXT_ALIGN_RIGHT, Sem::OnSurface, m1_);
    d.textInBox(r2, FONT_SIZE_6PT, TEXT_ALIGN_RIGHT, Sem::OnSurfaceMuted, m2_);
    d.textInBox(r3, FONT_SIZE_8PT, TEXT_ALIGN_RIGHT, Sem::OnSurface, m3_);
}

} // namespace ortxui
