/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/AboutView.hpp"
#include "core/Event.hpp"
#include "interfaces/keyboard.h"
#include "hwconfig.h"

namespace ortxui
{

/* Contributor credits (borrowed static table; mirrors the classic UI). */
static const char *const kAuthors[] = {
    "Niccolo' IU2KIN", "Silvano IU2KWO", "Federico IU2NUO", "Fred IU2NRO",
    "Joseph VK7JS",    "Morgan ON4MOD",  "Marco DM4RCO",
};
static constexpr uint16_t kAuthorCount = sizeof(kAuthors) / sizeof(kAuthors[0]);

void AboutView::build()
{
    const int16_t W = CONFIG_SCREEN_WIDTH;
    const int16_t H = CONFIG_SCREEN_HEIGHT;
    const bool regular = (sizeClass() == SizeClass::Regular);
    const int16_t barH = regular ? 16 : 12;

    root_.setArea({ 0, 0, (uint16_t)W, (uint16_t)H });
    root_.setAxis(Axis::Column);

    title_.init("About", barH);

    /* Brand + version block. */
    header_.setAxis(Axis::Column);
    header_.setAlign(Align::Stretch);
    header_.setJustify(Justify::Center);
    header_.setGap(1);
    header_.setBasis(regular ? 30 : 24);

    brand_.setFont(FONT_SIZE_8PT);
    brand_.setAlign(TEXT_ALIGN_CENTER);
    brand_.setColor(Sem::Primary);
    brand_.setText("OpenRTX");
    brand_.setBasis(gfx_getFontHeight(FONT_SIZE_8PT));

    version_.setFont(FONT_SIZE_6PT);
    version_.setAlign(TEXT_ALIGN_CENTER);
    version_.setColor(Sem::OnSurfaceMuted);
    version_.setText(GIT_VERSION);
    version_.setBasis(gfx_getFontHeight(FONT_SIZE_6PT));

    header_.addChild(&brand_);
    header_.addChild(&version_);

    /* Scrolling, non-selectable credits list fills the rest. */
    authors_.setItems(kAuthors, kAuthorCount);
    authors_.setRowHeight(regular ? 13 : 11);
    authors_.setSelectable(false);
    authors_.setGrow(1);

    root_.addChild(&title_);
    root_.addChild(&header_);
    root_.addChild(&authors_);
    screen_.addChild(&root_);
    root_.onLayout();

    screen_.markAllDirty();
}

NavIntent AboutView::onEvent(const Event &e)
{
    if ((e.kind == EvKind::Key) && ((e.keys & KEY_ESC) != 0u))
        return NavIntent::pop();

    /* Drive the credits list directly: it is nested in the layout tree, out of
     * reach of the Screen's (direct-child-only) focus ring. */
    authors_.onEvent(e);
    return NavIntent::none();
}

} // namespace ortxui
