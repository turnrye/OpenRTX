/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/ChannelsView.hpp"
#include "core/Event.hpp"
#include "core/state.h"
#include "interfaces/keyboard.h"
#include "hwconfig.h"

extern "C" {
#include "interfaces/cps_io.h"
}

#include <cstring>

namespace ortxui
{

void ChannelsView::build()
{
    const int16_t W = CONFIG_SCREEN_WIDTH;
    const int16_t H = CONFIG_SCREEN_HEIGHT;
    const bool regular = (sizeClass() == SizeClass::Regular);

    root_.setArea({ 0, 0, (uint16_t)W, (uint16_t)H });
    root_.setAxis(Axis::Column);

    topBar_.init("Channels");
    setTopBar(&topBar_);

    list_.setRowHeight(regular ? 16 : 12);
    list_.setFlag(FLAG_FOCUSABLE, true);
    list_.setGrow(1);

    empty_.setFont(regular ? FONT_SIZE_10PT : FONT_SIZE_8PT);
    empty_.setAlign(TEXT_ALIGN_CENTER);
    empty_.setColor(Sem::OnSurfaceMuted);
    empty_.setText("No channels");
    empty_.setGrow(1);

    root_.addChild(&topBar_);
    root_.addChild(&list_);
    root_.addChild(&empty_);
    screen_.addChild(&root_);

    reload();
}

void ChannelsView::reload()
{
    count_ = 0;
    for (uint16_t i = 0; i < kMaxRows; i++) {
        channel_t ch;
        if (cps_readChannel(&ch, i) != 0)
            break;
        strncpy(names_[i], ch.name, CPS_STR_SIZE - 1);
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

void ChannelsView::onShow()
{
    /* Reload each time the browser is opened (the codeplug is read lazily and
     * may not have been open when build() ran). */
    reload();
}

NavIntent ChannelsView::onEvent(const Event &e)
{
    if (e.kind == EvKind::Key) {
        if ((e.keys & KEY_ESC) != 0u)
            return NavIntent::pop();

        if (((e.keys & KEY_ENTER) != 0u) && (count_ > 0)) {
            const uint16_t sel = list_.selected();
            channel_t ch;
            if (cps_readChannel(&ch, sel) == 0) {
                state.channel = ch;
                state.channel_index = sel;
                requestSyncRtx();
                return NavIntent::popToRoot();
            }
            return NavIntent::none();
        }
    }

    screen_.dispatch(e);
    return NavIntent::none();
}

} // namespace ortxui
