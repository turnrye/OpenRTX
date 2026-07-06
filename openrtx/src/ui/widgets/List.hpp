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
 * A single row of a List. The row style follows the fields set:
 *  - label only            -> a plain row (submenu / credits),
 *  - label + value         -> a settings row (label top-left, value low-right),
 *  - label + checkbox      -> a checklist row (box + tick when checked).
 * All strings are borrowed; the owner keeps them (and the item array) alive.
 */
struct ListItem {
    const char *label = "";
    const char *value = nullptr;
    bool checkbox = false;
    bool checked = false;
};

/**
 * An on-demand source of list rows. Only the currently visible window is
 * queried (during draw), so a large list — e.g. a codeplug's hundreds of
 * channels — needs no per-row storage: the model reads each row lazily.
 *
 * rowAt() may point out.label at a scratch buffer that is only valid until the
 * next rowAt() call; the List draws each row immediately after fetching it.
 */
class ListModel
{
public:
    virtual ~ListModel() = default;
    virtual uint16_t rowCount() const = 0;
    virtual void rowAt(uint16_t index, ListItem &out) const = 0;
};

/**
 * A vertical, single-selection list of rows (mockups 2-4).
 *
 * The whole list is one focusable Object owning the selected index and scroll
 * offset, so a menu of any length stays within the Screen focus ring and maps
 * to the rotary knob / arrow keys. Encoder steps and UP/DOWN move the selection
 * (wrapping) and scroll to keep it visible; ENTER/ESC are left for the owning
 * view (which reads selected()/item() to navigate or toggle). The selected row
 * is a solid blue fill with black text; rows are separated by thin dividers.
 *
 * The item table is borrowed, not copied, matching the no-heap discipline.
 */
class List : public Object
{
public:
    /** Back the list with a borrowed static item array (small, fixed menus). */
    void setItems(const ListItem *items, uint16_t count);
    /** Back the list with an on-demand model (large / lazily-read lists). */
    void setModel(const ListModel *model);
    void setRowHeight(int16_t h)
    {
        rowH_ = h;
    }
    void setSelectable(bool s)
    {
        selectable_ = s;
    }

    uint16_t selected() const
    {
        return selected_;
    }
    void setSelected(uint16_t i);

    /** Mutable access to the selected item (e.g. to toggle a checkbox). */
    ListItem *selectedItem();

    /** Speak the current row via voice prompts (no-op unless vpLevel >= vpLow).
     * Called automatically on selection change; views call it on entry too. */
    void announceSelection() const;

    bool onEvent(const Event &e) override;
    void draw(DrawCtx &d) override;

private:
    uint16_t visibleRows() const;
    void scrollToSelected();
    void moveSelection(int dir);
    void drawRow(DrawCtx &d, const ListItem &it, const Rect &row,
                 bool selected);

    /** Effective row count from whichever backing (model or static array). */
    uint16_t rows() const
    {
        return (model_ != nullptr) ? model_->rowCount() : count_;
    }

    const ListItem *items_ = nullptr;
    const ListModel *model_ = nullptr;
    uint16_t count_ = 0;
    uint16_t selected_ = 0;
    uint16_t top_ = 0; //< index of the first visible row (scroll offset)
    int16_t rowH_ = 14;
    bool selectable_ = true;
};

} // namespace ortxui

#endif /* ORTX_UI_LIST_HPP */
