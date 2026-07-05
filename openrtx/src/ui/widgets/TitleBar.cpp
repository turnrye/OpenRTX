/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "widgets/TitleBar.hpp"

namespace ortxui
{

void TitleBar::init(const char *title, int16_t height)
{
    setAxis(Axis::Row);
    setAlign(Align::Stretch);
    setBackground(Sem::Surface);
    setBasis(height);

    label_.setFont(FONT_SIZE_8PT);
    label_.setAlign(TEXT_ALIGN_CENTER);
    label_.setColor(Sem::Primary);
    label_.setText(title);

    addChild(&label_);
}

} // namespace ortxui
