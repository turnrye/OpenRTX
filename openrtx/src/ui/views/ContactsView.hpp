/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_CONTACTSVIEW_HPP
#define ORTX_UI_CONTACTSVIEW_HPP

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
 * The main-menu Contacts browser: a scrollable list of the codeplug's contact
 * names (read on entry via cps_readContact). A read-only viewer, mirroring the
 * classic Contacts menu — ENTER has no action, ESC goes back. When the codeplug
 * has no contacts a centred "No contacts" line is shown instead.
 *
 * Same fixed-buffer caveat as ChannelsView (see there): a callback-backed List
 * is the proper fix for very large codeplugs.
 */
class ContactsView : public View
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

#endif /* ORTX_UI_CONTACTSVIEW_HPP */
