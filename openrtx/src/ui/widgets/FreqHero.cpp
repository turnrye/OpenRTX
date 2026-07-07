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
    /* Sized so the frequency to kHz PLUS the small trailing sub-kHz digits and
     * the mode stack all fit across the width (a 16pt main left no room for the
     * sub-kHz digits, which were then dropped). */
    const fontSize_t mainFont = compact ? FONT_SIZE_10PT : FONT_SIZE_12PT;
    const fontSize_t subFont = compact ? FONT_SIZE_6PT : FONT_SIZE_8PT;
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
    const uint16_t subW = gfx_getTextWidth(subFont, subBuf_);
    if (subX + subW <= modeLeft - 2) {
        const Point subAt = { subX, baseY };
        d.text(subAt, subFont, TEXT_ALIGN_LEFT, Sem::OnSurface, subBuf_);
    }

    /* Right-aligned mode stack: mode/bandwidth over the PL tone, vertically
     * centred with padding so it doesn't crowd the top edge. Power is omitted
     * (it is shown on the meter while transmitting). */
    const int16_t mx = modeLeft;
    const uint16_t mw = static_cast<uint16_t>(modeW);
    const fontSize_t modeFont = compact ? FONT_SIZE_6PT : FONT_SIZE_8PT;
    const int16_t h1 = compact ? 12 : 16; //< mode line height
    const int16_t h2 = compact ? 11 : 13; //< tone line height
    const int16_t gap = 2;
    const bool hasTone = (m2_[0] != '\0');
    const int16_t blockH = static_cast<int16_t>(hasTone ? h1 + gap + h2 : h1);
    const int16_t top = static_cast<int16_t>(area_.y + (area_.h - blockH) / 2);

    const Rect r1 = { mx, top, mw, static_cast<uint16_t>(h1) };
    d.textInBox(r1, modeFont, TEXT_ALIGN_RIGHT, Sem::OnSurface, m1_);
    if (hasTone) {
        const Rect r2 = { mx, static_cast<int16_t>(top + h1 + gap), mw,
                          static_cast<uint16_t>(h2) };
        d.textInBox(r2, FONT_SIZE_6PT, TEXT_ALIGN_RIGHT, Sem::OnSurfaceMuted,
                    m2_);
    }
}

} // namespace ortxui
