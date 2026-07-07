/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/TextInputView.hpp"
#include "core/Event.hpp"
#include "interfaces/keyboard.h"
#include "core/state.h"
#include "core/voicePrompts.h"
#include "core/voicePromptUtils.h"
#include "hwconfig.h"

#include <cstring>

namespace ortxui
{

void TextInputView::build()
{
    const int16_t W = CONFIG_SCREEN_WIDTH;
    const int16_t H = CONFIG_SCREEN_HEIGHT;
    const bool regular = (sizeClass() == SizeClass::Regular);

    root_.setArea({ 0, 0, (uint16_t)W, (uint16_t)H });
    root_.setAxis(Axis::Column);
    root_.setPadding(regular ? 4 : 2);
    root_.setGap(regular ? 3 : 2);

    title_.setColor(Sem::Accent);
    title_.setFont(regular ? FONT_SIZE_8PT : FONT_SIZE_6PT);
    title_.setAlign(TEXT_ALIGN_CENTER);
    title_.setBasis(regular ? 14 : 10);

    field_.setGrow(1); /* the scrolling field fills the middle */

    hint_.setText(regular ? "Up/Dn:char  L/R:move  #:del" : "Up/Dn L/R #");
    hint_.setColor(Sem::OnSurfaceMuted);
    hint_.setFont(regular ? FONT_SIZE_6PT : FONT_SIZE_5PT);
    hint_.setAlign(TEXT_ALIGN_CENTER);
    hint_.setBasis(regular ? 10 : 8);

    root_.addChild(&title_);
    root_.addChild(&field_);
    root_.addChild(&hint_);
    screen_.addChild(&root_);
    root_.onLayout();

    screen_.markAllDirty();
}

void TextInputView::open(const char *title, char *dst, uint16_t cap,
                         const Charset &cs, bool multiline, bool rtx)
{
    dst_ = dst;
    rtx_ = rtx;

    /* Copy into the working buffer, bounded by both the caller's capacity and
     * our own storage. */
    uint16_t lim = (cap < sizeof(work_)) ? cap : (uint16_t)sizeof(work_);
    if (lim == 0)
        lim = 1;
    cap_ = lim;
    strncpy(work_, (dst != nullptr) ? dst : "", lim - 1);
    work_[lim - 1] = '\0';

    title_.setText((title != nullptr) ? title : "");

    const bool regular = (sizeClass() == SizeClass::Regular);
    const fontSize_t f = regular ? FONT_SIZE_8PT : FONT_SIZE_6PT;
    field_.configure(work_, lim, cs,
                     multiline ? TextInput::Mode::MultiLine :
                                 TextInput::Mode::SingleLine,
                     TextInput::CursorStyle::Block, f);
    field_.begin();

    screen_.markAllDirty();
}

void TextInputView::announce()
{
    vpSay(title_.text());
}

void TextInputView::afterEdit()
{
    field_.invalidate();
    if (state.settings.vpLevel >= vpLow)
        vp_announceInputChar(field_.cursorChar());
}

void TextInputView::commit()
{
    field_.stripTrailingSpaces();
    if ((dst_ != nullptr) && (cap_ > 0)) {
        strncpy(dst_, work_, cap_ - 1);
        dst_[cap_ - 1] = '\0';
    }
    if (rtx_)
        requestSyncRtx();
}

NavIntent TextInputView::onEvent(const Event &e)
{
    if (e.kind == EvKind::Encoder) {
        field_.cycle(e.encoder);
        afterEdit();
        return NavIntent::none();
    }

    if (e.kind == EvKind::Key) {
        const uint32_t k = e.keys;
        if ((k & KEY_ENTER) != 0u) {
            commit();
            return NavIntent::pop();
        }
        if ((k & KEY_ESC) != 0u)
            return NavIntent::pop();
        if ((k & KEY_UP) != 0u)
            field_.cycle(+1);
        else if ((k & KEY_DOWN) != 0u)
            field_.cycle(-1);
        else if ((k & KEY_LEFT) != 0u)
            field_.moveCursor(-1);
        else if ((k & KEY_RIGHT) != 0u)
            field_.moveCursor(+1);
        else if ((k & KEY_HASH) != 0u)
            field_.backspace();
        else
            return NavIntent::none();
        afterEdit();
    }

    return NavIntent::none();
}

} // namespace ortxui
