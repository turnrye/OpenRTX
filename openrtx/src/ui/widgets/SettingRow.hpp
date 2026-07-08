/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_SETTINGROW_HPP
#define ORTX_UI_SETTINGROW_HPP

#include <cstdint>
#include "layout/Flex.hpp"
#include "widgets/Widgets.hpp"

namespace ortxui
{

/**
 * One List row, composed from the layout engine instead of hand-painted. A List
 * reuses a single SettingRow as a flyweight: it binds the row to each item in
 * turn, sizes it, lays it out and paints it. Because the row is a Flex column of
 * a label and (for a value row) a value line, the engine supplies the vertical
 * padding and inter-line gutter, and heightFor() reports the content height plus
 * a uniform inset — so a plain row, a checkbox row and a two-line value row all
 * carry the SAME padding to their separators, differing only in height.
 *
 * The row paints its own selection fill, bottom separator and (for a boolean)
 * an anti-aliased checkbox; the label / value children paint after draw().
 */
class SettingRow : public Flex
{
public:
    SettingRow();

    enum class Kind : uint8_t {
        Plain,    //< a single label (submenu / channel / credit line)
        Value,    //< label over a right-aligned value line
        Checkbox, //< a label with an anti-aliased tick box at the right
    };

    /** Per-list geometry: vertical content inset and the label/value gutter. */
    void setPad(int16_t padY, int16_t gap);

    /** Bind the flyweight to one row's content and selection state. */
    void bind(Kind kind, const char *label, const char *value, bool checked,
              bool selected);

    /** Row height for a given kind under the current padding — used by the List
     *  to lay rows out before binding their text. */
    int16_t heightFor(Kind kind) const;

    Size natural() const override;
    void draw(DrawCtx &d) override;

private:
    Label label_;
    Label value_;
    Kind kind_ = Kind::Plain;
    bool checked_ = false;
    bool selected_ = false;
    int16_t padY_ = 4;
    int16_t gap_ = 2;

    static constexpr int16_t kPadX = 6; //< left inset for the label
    /* Smaller fonts on the compact panels keep the list dense on a 64px
     * screen, mirroring the other size-class-aware widgets. */
    static constexpr fontSize_t kLabelFont =
        (sizeClass() == SizeClass::Regular) ? FONT_SIZE_8PT : FONT_SIZE_6PT;
    static constexpr fontSize_t kValueFont =
        (sizeClass() == SizeClass::Regular) ? FONT_SIZE_6PT : FONT_SIZE_5PT;
};

} // namespace ortxui

#endif /* ORTX_UI_SETTINGROW_HPP */
