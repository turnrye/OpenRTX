/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/BanksView.hpp"
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

void BanksView::build()
{
    const int16_t W = CONFIG_SCREEN_WIDTH;
    const int16_t H = CONFIG_SCREEN_HEIGHT;
    const bool regular = (sizeClass() == SizeClass::Regular);

    root_.setArea({ 0, 0, (uint16_t)W, (uint16_t)H });
    root_.setAxis(Axis::Column);

    topBar_.init("Banks");
    setTopBar(&topBar_);

    list_.setRowHeight(regular ? 16 : 12);
    list_.setFlag(FLAG_FOCUSABLE, true);
    list_.setGrow(1);

    root_.addChild(&topBar_);
    root_.addChild(&list_);
    screen_.addChild(&root_);

    reload();
}

void BanksView::reload()
{
    /* Row 0 is always "All channels" (clears the active bank). */
    strncpy(names_[0], "All channels", CPS_STR_SIZE - 1);
    names_[0][CPS_STR_SIZE - 1] = '\0';
    items_[0].label = names_[0];
    items_[0].value = nullptr;
    items_[0].checkbox = false;
    count_ = 1;

    for (uint16_t i = 0; (count_ < kMaxRows); i++) {
        bankHdr_t bank;
        if (cps_readBankHeader(&bank, i) != 0)
            break;
        strncpy(names_[count_], bank.name, CPS_STR_SIZE - 1);
        names_[count_][CPS_STR_SIZE - 1] = '\0';
        items_[count_].label = names_[count_];
        items_[count_].value = nullptr;
        items_[count_].checkbox = false;
        count_++;
    }

    list_.setItems(items_, count_);
    list_.setSelected(0);
    root_.onLayout();
    screen_.focusFirst();
    screen_.markAllDirty();
}

void BanksView::onShow()
{
    /* Reload on open — the codeplug may not have been read when build() ran. */
    reload();
}

NavIntent BanksView::onEvent(const Event &e)
{
    if (e.kind == EvKind::Key) {
        if ((e.keys & KEY_ESC) != 0u)
            return NavIntent::pop();

        if ((e.keys & KEY_ENTER) != 0u) {
            const uint16_t sel = list_.selected();
            channel_t ch;
            bool loaded = false;

            if (sel == 0) {
                /* All channels: clear the bank filter, load global channel 0. */
                state.bank_enabled = false;
                if (cps_readChannel(&ch, 0) == 0) {
                    state.channel = ch;
                    state.channel_index = 0;
                    loaded = true;
                }
            } else {
                const uint16_t bankPos = (uint16_t)(sel - 1);
                bankHdr_t bank;
                if (cps_readBankHeader(&bank, bankPos) == 0) {
                    state.bank_enabled = true;
                    state.bank = bankPos;
                    /* Load the bank's first channel (its global index). */
                    const int g = cps_readBankData(bankPos, 0);
                    if ((g >= 0) && (cps_readChannel(&ch, (uint16_t)g) == 0)) {
                        state.channel = ch;
                        state.channel_index = 0;
                        loaded = true;
                    }
                }
            }

            if (loaded)
                requestSyncRtx();
            return NavIntent::popToRoot();
        }
    }

    screen_.dispatch(e);
    return NavIntent::none();
}

} // namespace ortxui
