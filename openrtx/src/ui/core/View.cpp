/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/View.hpp"
#include "core/Event.hpp"
#include "widgets/TopBar.hpp"
#include "interfaces/keyboard.h"
#include "core/voicePrompts.h"
#include "core/voicePromptUtils.h"

namespace ortxui
{

void View::syncFromState(const state_t &s)
{
    if (topBar_ != nullptr)
        topBar_->update(s);
}

void View::vpSay(const char *text)
{
    if ((state.settings.vpLevel < vpLow) || (text == nullptr)
        || (text[0] == '\0'))
        return;

    vp_flush();
    vp_announceText(text, vp_getVoiceLevelQueueFlags());
    vp_play();
}

NavIntent View::onEvent(const Event &e)
{
    if ((e.kind == EvKind::Key) && ((e.keys & KEY_ESC) != 0u))
        return NavIntent::pop();

    screen().dispatch(e);
    return NavIntent::none();
}

} // namespace ortxui
