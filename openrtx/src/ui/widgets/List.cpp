/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "widgets/List.hpp"
#include "core/Event.hpp"
#include "render/DrawCtx.hpp"
#include "interfaces/keyboard.h"

namespace ortxui
{

void List::setItems(const char *const *items, uint16_t count)
{
    items_ = items;
    count_ = count;
    selected_ = 0;
    top_ = 0;
    invalidate();
}

uint16_t List::visibleRows() const
{
    if (rowH_ <= 0)
        return 1;

    const uint16_t v = static_cast<uint16_t>(area_.h / rowH_);
    return (v == 0) ? 1 : v;
}

void List::scrollToSelected()
{
    const uint16_t vis = visibleRows();
    if (selected_ < top_)
        top_ = selected_;
    else if (selected_ >= static_cast<uint16_t>(top_ + vis))
        top_ = static_cast<uint16_t>(selected_ - vis + 1);
}

void List::setSelected(uint16_t i)
{
    if (count_ == 0)
        return;
    if (i >= count_)
        i = static_cast<uint16_t>(count_ - 1);
    if (i == selected_)
        return;

    selected_ = i;
    scrollToSelected();
    invalidate();
}

void List::moveSelection(int dir)
{
    if (count_ == 0)
        return;

    const int n = static_cast<int>(count_);
    int idx = static_cast<int>(selected_) + ((dir >= 0) ? 1 : -1);
    if (idx < 0)
        idx = n - 1; /* wrap to the bottom */
    else if (idx >= n)
        idx = 0;     /* wrap to the top */

    selected_ = static_cast<uint16_t>(idx);
    scrollToSelected();
    invalidate();
}

bool List::onEvent(const Event &e)
{
    if (e.kind == EvKind::Encoder) {
        moveSelection(e.encoder);
        return true;
    }
    if (e.kind == EvKind::Key) {
        if ((e.keys & KEY_DOWN) != 0u) {
            moveSelection(1);
            return true;
        }
        if ((e.keys & KEY_UP) != 0u) {
            moveSelection(-1);
            return true;
        }
    }
    return false;
}

void List::draw(DrawCtx &d)
{
    if ((count_ == 0) || (items_ == nullptr))
        return;

    const uint16_t vis = visibleRows();
    uint16_t end = static_cast<uint16_t>(top_ + vis);
    if (end > count_)
        end = count_;

    const int16_t fh = static_cast<int16_t>(gfx_getFontHeight(FONT_SIZE_8PT));

    for (uint16_t i = top_; i < end; i++) {
        const int16_t rowY = static_cast<int16_t>(area_.y + (i - top_) * rowH_);
        const Rect row = { area_.x, rowY, area_.w,
                           static_cast<uint16_t>(rowH_) };

        Sem textColor = Sem::OnSurface;
        if (selectable_ && (i == selected_)) {
            d.fillRect(row, Sem::SurfaceHigh);
            const Rect accent = { area_.x, rowY, 2,
                                  static_cast<uint16_t>(rowH_) };
            d.fillRect(accent, Sem::Primary);
            textColor = Sem::Primary;
        }

        /* Vertically centre the glyphs within the row (baseline anchor). */
        const int16_t ty = static_cast<int16_t>(rowY + (rowH_ + fh) / 2 - 1);
        const Point at = { static_cast<int16_t>(area_.x + 6), ty };
        d.text(at, FONT_SIZE_8PT, TEXT_ALIGN_LEFT, textColor, items_[i]);
    }

    /* Scroll indicator: a thin track with a thumb sized to the visible span. */
    if (count_ > vis) {
        const int16_t barX = static_cast<int16_t>(area_.right() - 1);
        const int trackH = static_cast<int>(area_.h);
        int thumbH = trackH * vis / count_;
        if (thumbH < 2)
            thumbH = 2;
        const int16_t thumbY =
            static_cast<int16_t>(area_.y + trackH * top_ / count_);

        const Rect track = { barX, area_.y, 2, area_.h };
        d.fillRect(track, Sem::Surface);
        const Rect thumb = { barX, thumbY, 2, static_cast<uint16_t>(thumbH) };
        d.fillRect(thumb, Sem::OnSurfaceMuted);
    }
}

} // namespace ortxui
