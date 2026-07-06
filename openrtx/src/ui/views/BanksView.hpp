/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_BANKSVIEW_HPP
#define ORTX_UI_BANKSVIEW_HPP

#include <cstdint>
#include "core/View.hpp"
#include "layout/Flex.hpp"
#include "widgets/TopBar.hpp"
#include "widgets/List.hpp"
#include "core/cps.h"

namespace ortxui
{

/**
 * The main-menu Banks browser: an "All channels" entry followed by the
 * codeplug's bank names (read on entry via cps_readBankHeader). ENTER on "All
 * channels" clears the active bank; ENTER on a bank enables it and loads its
 * first channel; either way it requests an rtx resync and unwinds to the VFO
 * home. ESC goes back. The list always has at least the "All channels" row, so
 * there is no empty state.
 *
 * Same fixed-buffer caveat as ChannelsView (see there).
 */
class BanksView : public View
{
public:
    void build();
    void onShow() override;
    NavIntent onEvent(const Event &e) override;

    Screen &screen() override
    {
        return screen_;
    }

private:
    static constexpr uint16_t kMaxRows = 64;

    void reload();

    Screen screen_;
    Flex root_;
    TopBar topBar_;
    List list_;

    ListItem items_[kMaxRows] = {};
    char names_[kMaxRows][CPS_STR_SIZE] = {};
    uint16_t count_ = 0;
};

} // namespace ortxui

#endif /* ORTX_UI_BANKSVIEW_HPP */
