/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_NAVIGATOR_HPP
#define ORTX_UI_NAVIGATOR_HPP

#include <cstdint>
#include "core/View.hpp"

namespace ortxui
{

struct Event;

/**
 * A depth-bounded stack of Views.
 *
 * ENTER-style actions push a child view, ESC pops back to the parent, and the
 * top of the stack is the active screen that ui_shim draws and feeds state to.
 * Activating a view marks it fully dirty so it repaints cleanly over whatever
 * screen it now covers. The root view is never popped, so ESC on the home
 * screen is a no-op. All storage is static (no heap): the stack is a small
 * fixed array of View pointers.
 */
class Navigator
{
public:
    /** Clear the stack and make `root` the (unpoppable) base screen. */
    void setRoot(View *root);

    /** Feed an event to the active view and apply its navigation intent. */
    void dispatch(const Event &e);

    /** Forward the state snapshot to the active view. */
    void syncActive(const state_t &s);

    /** The current top-of-stack view, or nullptr before setRoot(). */
    View *active() const
    {
        return (depth_ > 0) ? stack_[depth_ - 1] : nullptr;
    }

private:
    static constexpr uint8_t kMaxDepth = 8;

    void push(View *v);
    void pop();
    void activate();

    View *stack_[kMaxDepth] = {};
    uint8_t depth_ = 0;
};

} // namespace ortxui

#endif /* ORTX_UI_NAVIGATOR_HPP */
