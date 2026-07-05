/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/MenuView.hpp"
#include "core/Event.hpp"
#include "interfaces/keyboard.h"
#include "hwconfig.h"

namespace ortxui
{

void MenuView::build(const char *title, const char *const *items,
                     uint16_t count)
{
    const int16_t W = CONFIG_SCREEN_WIDTH;
    const int16_t H = CONFIG_SCREEN_HEIGHT;
    const int16_t topH = 16;

    /* Title bar */
    topBar_.setArea({ 0, 0, (uint16_t)W, (uint16_t)topH });
    topBar_.setColor(Sem::Surface);

    title_.setAnchor({ (int16_t)(W / 2), 11 });
    title_.setFont(FONT_SIZE_8PT);
    title_.setAlign(TEXT_ALIGN_CENTER);
    title_.setColor(Sem::Primary);
    title_.setText(title);

    /* Scrollable list fills the space below the title bar. */
    list_.setArea({ 0, topH, (uint16_t)W, (uint16_t)(H - topH) });
    list_.setItems(items, count);
    list_.setFlag(FLAG_FOCUSABLE, true);

    screen_.addChild(&topBar_);
    screen_.addChild(&title_);
    screen_.addChild(&list_);
    screen_.focusFirst();
    screen_.markAllDirty();
}

void MenuView::setRowTarget(uint16_t row, View *target)
{
    if (row < kMaxRows)
        rowTargets_[row] = target;
}

NavIntent MenuView::onEvent(const Event &e)
{
    if (e.kind == EvKind::Key) {
        if ((e.keys & KEY_ESC) != 0u)
            return NavIntent::pop();

        if ((e.keys & KEY_ENTER) != 0u) {
            const uint16_t sel = list_.selected();
            View *target = (sel < kMaxRows) ? rowTargets_[sel] : nullptr;
            if (target != nullptr)
                return NavIntent::push(target);
            return NavIntent::none(); /* leaf row: nothing to open yet */
        }
    }

    /* Encoder / UP / DOWN reach the focused List and move the selection. */
    screen_.dispatch(e);
    return NavIntent::none();
}

} // namespace ortxui
