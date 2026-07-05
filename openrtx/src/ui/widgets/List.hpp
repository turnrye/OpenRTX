/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_LIST_HPP
#define ORTX_UI_LIST_HPP

#include <cstdint>
#include "core/Object.hpp"
#include "core/graphics.h"
#include "style/SemanticColor.hpp"

namespace ortxui
{

/**
 * A vertical, single-selection list of text rows.
 *
 * The whole list is one focusable Object: it owns the selected index and the
 * scroll offset itself rather than making each row a focusable child. That
 * keeps a menu of any length within the Screen's small focus ring and mirrors
 * how the radio's rotary knob / arrow keys actually move a selection. Encoder
 * steps and UP/DOWN move the selection (wrapping at the ends) and scroll to
 * keep it visible; ENTER/ESC are left unconsumed for the owning view to read
 * via selected() and turn into navigation.
 *
 * The item table is borrowed, not copied: the caller keeps the strings alive
 * (menu tables are static const), matching the toolkit's no-heap discipline.
 */
class List : public Object
{
public:
    void setItems(const char *const *items, uint16_t count);
    void setRowHeight(int16_t h)
    {
        rowH_ = h;
    }

    uint16_t selected() const
    {
        return selected_;
    }
    void setSelected(uint16_t i);

    bool onEvent(const Event &e) override;
    void draw(DrawCtx &d) override;

private:
    uint16_t visibleRows() const;
    void scrollToSelected();
    void moveSelection(int dir);

    const char *const *items_ = nullptr;
    uint16_t count_ = 0;
    uint16_t selected_ = 0;
    uint16_t top_ = 0; //< index of the first visible row (scroll offset)
    int16_t rowH_ = 14;
};

} // namespace ortxui

#endif /* ORTX_UI_LIST_HPP */
