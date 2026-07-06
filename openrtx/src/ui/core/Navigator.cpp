/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/Navigator.hpp"
#include "core/Event.hpp"

namespace ortxui
{

void Navigator::setRoot(View *root)
{
    depth_ = 0;
    if (root != nullptr) {
        stack_[depth_++] = root;
        activate();
    }
}

void Navigator::push(View *v)
{
    if ((v == nullptr) || (depth_ >= kMaxDepth))
        return;

    stack_[depth_++] = v;
    activate();
}

void Navigator::pop()
{
    if (depth_ <= 1)
        return; /* the root view is never popped */

    depth_--;
    activate();
}

void Navigator::activate()
{
    View *v = active();
    if (v == nullptr)
        return;

    v->onShow();
    v->screen().markAllDirty();
}

void Navigator::dispatch(const Event &e)
{
    View *v = active();
    if (v == nullptr)
        return;

    const NavIntent intent = v->onEvent(e);
    switch (intent.action) {
        case NavAction::Push:
            push(intent.target);
            break;
        case NavAction::Pop:
            pop();
            break;
        case NavAction::PopToRoot:
            popToRoot();
            break;
        case NavAction::None:
            break;
    }
}

void Navigator::popToRoot()
{
    if (depth_ <= 1)
        return;

    depth_ = 1;
    activate();
}

void Navigator::syncActive(const state_t &s)
{
    View *v = active();
    if (v != nullptr)
        v->syncFromState(s);
}

} // namespace ortxui
