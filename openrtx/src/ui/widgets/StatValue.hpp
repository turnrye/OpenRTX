/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_STATVALUE_HPP
#define ORTX_UI_STATVALUE_HPP

#include <cstdint>
#include "core/Object.hpp"
#include "style/SemanticColor.hpp"
#include "core/graphics.h"

namespace ortxui
{

/**
 * A right-aligned "big value + small unit" readout (GPS mockup 1): a large
 * value sits at the right edge with a small, muted unit/label to its right
 * (e.g. "33°50.42'" + "N", "150m" + "ALT"). Both are vertically centred in the
 * object's box; the value column shrinks to leave room for the unit. The
 * strings are copied so callers may reuse their formatting buffer.
 */
class StatValue : public Object
{
public:
    void setValue(const char *v);
    void setUnit(const char *u);
    void setValueFont(fontSize_t f)
    {
        valueFont_ = f;
    }
    void setColors(Sem value, Sem unit)
    {
        valueColor_ = value;
        unitColor_ = unit;
    }

    void draw(DrawCtx &d) override;

private:
    char value_[16] = "";
    char unit_[6] = "";
    fontSize_t valueFont_ = FONT_SIZE_10PT;
    Sem valueColor_ = Sem::OnSurface;
    Sem unitColor_ = Sem::OnSurfaceMuted;
};

} // namespace ortxui

#endif /* ORTX_UI_STATVALUE_HPP */
