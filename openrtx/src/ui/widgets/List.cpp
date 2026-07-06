/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "widgets/List.hpp"
#include "core/Event.hpp"
#include "render/DrawCtx.hpp"
#include "interfaces/keyboard.h"
#include "core/state.h"
#include "core/voicePrompts.h"
#include "core/voicePromptUtils.h"

namespace ortxui
{

void List::setItems(const ListItem *items, uint16_t count)
{
    items_ = items;
    model_ = nullptr;
    count_ = count;
    selected_ = 0;
    top_ = 0;
    invalidate();
}

void List::setModel(const ListModel *model)
{
    model_ = model;
    items_ = nullptr;
    count_ = 0;
    selected_ = 0;
    top_ = 0;
    invalidate();
}

ListItem *List::selectedItem()
{
    /* Model-backed rows are read-only (fetched into a scratch on draw), so
     * there is no persistent item to mutate; callers read selected() instead. */
    if ((items_ == nullptr) || (selected_ >= count_))
        return nullptr;
    /* The item table is borrowed and owned mutably by the view. */
    return const_cast<ListItem *>(&items_[selected_]);
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
    const uint16_t n = rows();
    if (n == 0)
        return;
    if (i >= n)
        i = static_cast<uint16_t>(n - 1);
    if (i == selected_)
        return;

    selected_ = i;
    scrollToSelected();
    invalidate();
    announceSelection();
}

void List::moveSelection(int dir)
{
    const int n = static_cast<int>(rows());
    if (n == 0)
        return;
    int idx = static_cast<int>(selected_) + ((dir >= 0) ? 1 : -1);
    if (idx < 0)
        idx = n - 1; /* wrap to the bottom */
    else if (idx >= n)
        idx = 0;     /* wrap to the top */

    selected_ = static_cast<uint16_t>(idx);
    scrollToSelected();
    invalidate();
    announceSelection();
}

void List::announceSelection() const
{
    /* Speak the freshly-selected row (label, then value or checkbox state) when
     * voice prompts are enabled — the one place that covers navigation in every
     * list-backed view. Dormant at the default vpNone level. */
    if (!selectable_ || (state.settings.vpLevel < vpLow))
        return;

    const uint16_t n = rows();
    if (selected_ >= n)
        return;

    ListItem it;
    if (model_ != nullptr)
        model_->rowAt(selected_, it);
    else if (items_ != nullptr)
        it = items_[selected_];
    else
        return;

    if ((it.label == nullptr) || (it.label[0] == '\0'))
        return;

    const enum vpQueueFlags flags = vp_getVoiceLevelQueueFlags();
    vp_flush();
    vp_announceText(it.label, flags);
    if (it.checkbox)
        vp_announceText(it.checked ? "On" : "Off", vpqDefault);
    else if ((it.value != nullptr) && (it.value[0] != '\0'))
        vp_announceText(it.value, vpqDefault);
    vp_play();
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

void List::drawRow(DrawCtx &d, const ListItem &it, const Rect &row,
                   bool selected)
{
    const Sem labelColor = selected ? Sem::OnPrimary : Sem::OnSurface;
    const Sem valueColor = selected ? Sem::OnPrimary : Sem::OnSurfaceMuted;

    /* A checkbox reserves room on the right; keep text clear of it. */
    const int16_t rightPad = it.checkbox ? 20 : 4;
    const int16_t textW = static_cast<int16_t>(row.w - 6 - rightPad);

    if (it.value != nullptr) {
        /* Value row: label on the upper line, value low and right-aligned. */
        const int16_t half = static_cast<int16_t>(row.h / 2);
        const Rect lbox = { static_cast<int16_t>(row.x + 6), row.y,
                            static_cast<uint16_t>(textW),
                            static_cast<uint16_t>(half + 2) };
        d.textInBox(lbox, FONT_SIZE_8PT, TEXT_ALIGN_LEFT, labelColor, it.label);

        const Rect vbox = { static_cast<int16_t>(row.x + 6),
                            static_cast<int16_t>(row.y + half - 2),
                            static_cast<uint16_t>(textW),
                            static_cast<uint16_t>(row.h - half) };
        d.textInBox(vbox, FONT_SIZE_6PT, TEXT_ALIGN_RIGHT, valueColor,
                    it.value);
    } else {
        const Rect lbox = { static_cast<int16_t>(row.x + 6), row.y,
                            static_cast<uint16_t>(textW), row.h };
        d.textInBox(lbox, FONT_SIZE_8PT, TEXT_ALIGN_LEFT, labelColor, it.label);
    }

    if (it.checkbox) {
        const int16_t bs = 12; /* box side */
        const int16_t bx = static_cast<int16_t>(row.right() - bs - 4);
        const int16_t by = static_cast<int16_t>(row.y + (row.h - bs) / 2);
        const Rect box = { bx, by, static_cast<uint16_t>(bs),
                           static_cast<uint16_t>(bs) };
        d.drawRect(box, selected ? Sem::OnPrimary : Sem::OnSurfaceMuted);

        if (it.checked) {
            /* A two-stroke tick in the mark colour. */
            const Point p0 = { static_cast<int16_t>(bx + 2),
                               static_cast<int16_t>(by + bs / 2) };
            const Point p1 = { static_cast<int16_t>(bx + bs / 2 - 1),
                               static_cast<int16_t>(by + bs - 3) };
            const Point p2 = { static_cast<int16_t>(bx + bs - 2),
                               static_cast<int16_t>(by + 2) };
            d.line(p0, p1, Sem::Mark);
            d.line(p1, p2, Sem::Mark);
        }
    }
}

void List::draw(DrawCtx &d)
{
    const uint16_t n = rows();
    if ((n == 0) || ((items_ == nullptr) && (model_ == nullptr)))
        return;

    const uint16_t vis = visibleRows();
    uint16_t end = static_cast<uint16_t>(top_ + vis);
    if (end > n)
        end = n;

    for (uint16_t i = top_; i < end; i++) {
        const int16_t rowY = static_cast<int16_t>(area_.y + (i - top_) * rowH_);
        const Rect row = { area_.x, rowY, area_.w,
                           static_cast<uint16_t>(rowH_) };

        /* Divider at the row's baseline; a selected row's fill covers its own. */
        const Rect sep = { area_.x, static_cast<int16_t>(row.bottom()), area_.w,
                           1 };
        d.fillRect(sep, Sem::Separator);

        const bool selected = selectable_ && (i == selected_);
        if (selected)
            d.fillRect(row, Sem::Primary);

        /* Model rows are fetched into a scratch and drawn immediately. */
        if (model_ != nullptr) {
            ListItem tmp;
            model_->rowAt(i, tmp);
            drawRow(d, tmp, row, selected);
        } else {
            drawRow(d, items_[i], row, selected);
        }
    }

    /* Scroll indicator: a thin track with a thumb sized to the visible span. */
    if (n > vis) {
        const int16_t barX = static_cast<int16_t>(area_.right() - 1);
        const int trackH = static_cast<int>(area_.h);
        int thumbH = trackH * vis / n;
        if (thumbH < 2)
            thumbH = 2;
        const int16_t thumbY =
            static_cast<int16_t>(area_.y + trackH * top_ / n);

        const Rect track = { barX, area_.y, 2, area_.h };
        d.fillRect(track, Sem::Surface);
        const Rect thumb = { barX, thumbY, 2, static_cast<uint16_t>(thumbH) };
        d.fillRect(thumb, Sem::OnSurfaceMuted);
    }
}

} // namespace ortxui
