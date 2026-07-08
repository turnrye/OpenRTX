/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "widgets/BatteryIcon.hpp"
#include "render/DrawCtx.hpp"
#include "core/Layout.hpp"

namespace ortxui
{

Size BatteryIcon::natural() const
{
    /* A vertical (upright) battery with the terminal on top, sized to sit in
     * the top bar; draw() fits within whatever area the row assigns. */
    const bool regular = (sizeClass() == SizeClass::Regular);
    return regular ? Size{ 14, 15 } : Size{ 11, 11 };
}

void BatteryIcon::draw(DrawCtx &d)
{
    if (area_.empty())
        return;

    const int16_t nubH = 2;
    const int16_t bodyH = static_cast<int16_t>(area_.h - nubH);
    if (bodyH < 6)
        return;

    /* Battery aspect: taller than wide. Centre it in the assigned width. */
    int16_t W = static_cast<int16_t>(bodyH * 65 / 100);
    if (W > static_cast<int16_t>(area_.w))
        W = static_cast<int16_t>(area_.w);
    if (W < 6)
        W = 6;

    const int16_t bx = static_cast<int16_t>(area_.x + (area_.w - W) / 2);
    const int16_t byNub = area_.y;
    const int16_t byBody = static_cast<int16_t>(area_.y + nubH);
    const uint16_t r = (W >= 10) ? 2u : 1u;

    /* Terminal nub, centred on top of the body. */
    const int16_t nubW = static_cast<int16_t>(W / 2);
    d.fillRoundRect({ static_cast<int16_t>(bx + (W - nubW) / 2), byNub,
                      static_cast<uint16_t>(nubW),
                      static_cast<uint16_t>(nubH + 1) },
                    1, Sem::OnSurface);

    /* Body outline = an outer rounded rect hollowed by an inner one in the bar
     * colour (the top bar's Surface), leaving an anti-aliased rounded ring. */
    d.fillRoundRect({ bx, byBody, static_cast<uint16_t>(W),
                      static_cast<uint16_t>(bodyH) },
                    r, Sem::OnSurface);
    d.fillRoundRect(
        { static_cast<int16_t>(bx + 1), static_cast<int16_t>(byBody + 1),
          static_cast<uint16_t>(W - 2), static_cast<uint16_t>(bodyH - 2) },
        static_cast<uint16_t>(r > 0 ? r - 1 : 0), Sem::Surface);

    /* Charge fill, from the bottom up, inset 2px from the outline. */
    const int innerH = bodyH - 4;
    const int fillH = innerH * static_cast<int>(charge_) / 100;
    if (fillH <= 0)
        return;

    const Sem fill = (charge_ <= 20u) ? Sem::Warning : Sem::OnSurface;
    d.fillRoundRect({ static_cast<int16_t>(bx + 2),
                      static_cast<int16_t>(byBody + 2 + (innerH - fillH)),
                      static_cast<uint16_t>(W - 4),
                      static_cast<uint16_t>(fillH) },
                    1, fill);
}

} // namespace ortxui
