/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "widgets/BatteryIcon.hpp"
#include "render/DrawCtx.hpp"

namespace ortxui
{

Size BatteryIcon::natural() const
{
    return { 22, 11 };
}

void BatteryIcon::draw(DrawCtx &d)
{
    if (area_.empty())
        return;

    /* Reserve 2px on the right for the terminal nub. */
    const int16_t bodyW = static_cast<int16_t>(area_.w - 2);
    if (bodyW <= 2)
        return;

    const Rect body = { area_.x, area_.y, static_cast<uint16_t>(bodyW),
                        area_.h };
    d.drawRect(body, Sem::OnSurface);

    /* Terminal nub, vertically centred on the right edge. */
    const int16_t nubH = static_cast<int16_t>(area_.h / 2);
    const Rect nub = { static_cast<int16_t>(area_.x + bodyW),
                       static_cast<int16_t>(area_.y + (area_.h - nubH) / 2), 2,
                       static_cast<uint16_t>(nubH) };
    d.fillRect(nub, Sem::OnSurface);

    /* Inner fill proportional to charge, inset 2px from the outline. */
    const int innerW = bodyW - 4;
    if (innerW <= 0)
        return;

    const int fillW = innerW * charge_ / 100;
    if (fillW <= 0)
        return;

    const Sem fill = (charge_ <= 20u) ? Sem::Warning : Sem::OnSurface;
    const Rect inner = { static_cast<int16_t>(area_.x + 2),
                         static_cast<int16_t>(area_.y + 2),
                         static_cast<uint16_t>(fillW),
                         static_cast<uint16_t>(area_.h - 4) };
    d.fillRect(inner, fill);
}

} // namespace ortxui
