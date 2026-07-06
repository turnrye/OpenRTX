/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/ContactsView.hpp"
#include "core/Event.hpp"
#include "interfaces/keyboard.h"
#include "hwconfig.h"

extern "C" {
#include "interfaces/cps_io.h"
}

#include <cstring>

namespace ortxui
{

void ContactsView::build()
{
    const int16_t W = CONFIG_SCREEN_WIDTH;
    const int16_t H = CONFIG_SCREEN_HEIGHT;
    const bool regular = (sizeClass() == SizeClass::Regular);

    root_.setArea({ 0, 0, (uint16_t)W, (uint16_t)H });
    root_.setAxis(Axis::Column);

    topBar_.init("Contacts");
    setTopBar(&topBar_);

    list_.setRowHeight(regular ? 16 : 12);
    list_.setFlag(FLAG_FOCUSABLE, true);
    list_.setGrow(1);

    empty_.setFont(regular ? FONT_SIZE_10PT : FONT_SIZE_8PT);
    empty_.setAlign(TEXT_ALIGN_CENTER);
    empty_.setColor(Sem::OnSurfaceMuted);
    empty_.setText("No contacts");
    empty_.setGrow(1);

    root_.addChild(&topBar_);
    root_.addChild(&list_);
    root_.addChild(&empty_);
    screen_.addChild(&root_);

    reload();
}

void ContactsView::reload()
{
    count_ = 0;
    for (uint16_t i = 0; i < kMaxRows; i++) {
        contact_t ct;
        if (cps_readContact(&ct, i) != 0)
            break;
        strncpy(names_[i], ct.name, CPS_STR_SIZE - 1);
        names_[i][CPS_STR_SIZE - 1] = '\0';
        items_[i].label = names_[i];
        items_[i].value = nullptr;
        items_[i].checkbox = false;
        count_++;
    }

    const bool empty = (count_ == 0);
    list_.setItems(items_, count_);
    list_.setSelected(0);
    list_.setFlag(FLAG_HIDDEN, empty);
    empty_.setFlag(FLAG_HIDDEN, !empty);

    root_.onLayout();
    if (!empty)
        screen_.focusFirst();
    screen_.markAllDirty();
}

void ContactsView::onShow()
{
    /* Reload on open — the codeplug may not have been read when build() ran. */
    reload();
}

NavIntent ContactsView::onEvent(const Event &e)
{
    if ((e.kind == EvKind::Key) && ((e.keys & KEY_ESC) != 0u))
        return NavIntent::pop();

    /* Read-only viewer: ENTER has no action (matches the classic UI); the List
     * still scrolls on UP/DOWN/encoder via the Screen focus ring. */
    screen_.dispatch(e);
    return NavIntent::none();
}

} // namespace ortxui
