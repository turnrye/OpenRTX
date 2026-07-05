/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_TITLEBAR_HPP
#define ORTX_UI_TITLEBAR_HPP

#include <cstdint>
#include "layout/Flex.hpp"
#include "widgets/Widgets.hpp"

namespace ortxui
{

/**
 * A screen header: a Surface-backed row with a centred title, used as the top
 * item of a view's column. It is itself a Flex, so it lays out and paints as
 * part of the tree; init() gives it a fixed height (set as its layout basis) so
 * a parent column reserves the right band for it.
 */
class TitleBar : public Flex
{
public:
    void init(const char *title, int16_t height);
    void setTitle(const char *t)
    {
        label_.setText(t);
    }

private:
    Label label_;
};

} // namespace ortxui

#endif /* ORTX_UI_TITLEBAR_HPP */
