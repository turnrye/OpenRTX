/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_ABOUTVIEW_HPP
#define ORTX_UI_ABOUTVIEW_HPP

#include "core/View.hpp"
#include "layout/Flex.hpp"
#include "widgets/Widgets.hpp"
#include "widgets/TitleBar.hpp"
#include "widgets/List.hpp"

namespace ortxui
{

/**
 * The About screen: a title bar, the OpenRTX brand + firmware version, and a
 * scrolling (non-selectable) list of contributor credits. A leaf view — ESC
 * pops back, the knob / up-down scroll the credits.
 */
class AboutView : public View
{
public:
    void build();

    NavIntent onEvent(const Event &e) override;
    Screen &screen() override
    {
        return screen_;
    }

private:
    Screen screen_;
    Flex root_;
    TitleBar title_;
    Flex header_;
    Label brand_;
    Label version_;
    List authors_;
};

} // namespace ortxui

#endif /* ORTX_UI_ABOUTVIEW_HPP */
