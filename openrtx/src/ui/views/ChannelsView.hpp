/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_CHANNELSVIEW_HPP
#define ORTX_UI_CHANNELSVIEW_HPP

#include <cstdint>
#include "core/View.hpp"
#include "layout/Flex.hpp"
#include "widgets/Widgets.hpp"
#include "widgets/TopBar.hpp"
#include "widgets/List.hpp"
#include "core/cps.h"

namespace ortxui
{

/**
 * The main-menu Channels browser: a scrollable list of the codeplug's channel
 * names (read on entry via cps_readChannel). ENTER loads the selected channel
 * into state.channel, requests an rtx resync, and unwinds to the home screen;
 * ESC goes back. When the codeplug has no channels a centred "No channels"
 * line is shown instead of the list.
 *
 * NOTE: names are copied into a fixed buffer (kMaxRows) rather than rendered
 * on demand, so very large codeplugs are capped. A callback-backed List (like
 * the classic UI's per-row read) is the proper fix for big codeplugs.
 */
class ChannelsView : public View
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
    Label empty_;

    ListItem items_[kMaxRows] = {};
    char names_[kMaxRows][CPS_STR_SIZE] = {};
    uint16_t count_ = 0;
};

} // namespace ortxui

#endif /* ORTX_UI_CHANNELSVIEW_HPP */
