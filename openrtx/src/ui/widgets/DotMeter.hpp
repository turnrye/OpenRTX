/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_DOTMETER_HPP
#define ORTX_UI_DOTMETER_HPP

#include <cstdint>
#include "core/Object.hpp"
#include "style/SemanticColor.hpp"

namespace ortxui
{

/**
 * The VFO signal meter (mockups 5-6): a leading label ("RX"/"TX"), a row of
 * dots filled up to the current level (outline beyond), and a trailing readout.
 * The colour role carries the RX-green / TX-orange state. In RX the S-scale
 * numbers (3/5/7/9) are drawn at their dot positions; TX hides the scale and
 * reads as a solid power bar. Drawn within its layout-assigned area.
 */
class DotMeter : public Object
{
public:
    void setLabel(const char *l)
    {
        label_ = (l != nullptr) ? l : "";
    }
    void setReadout(const char *r)
    {
        readout_ = (r != nullptr) ? r : "";
    }
    void setColor(Sem c)
    {
        color_ = c;
    }
    void setLevel(uint8_t filled, uint8_t total)
    {
        filled_ = filled;
        total_ = total;
    }
    void setShowScale(bool s)
    {
        showScale_ = s;
    }

    Size natural() const override;
    void draw(DrawCtx &d) override;

private:
    const char *label_ = "";
    const char *readout_ = "";
    Sem color_ = Sem::RxSuccess;
    uint8_t filled_ = 0;
    uint8_t total_ = 9;
    bool showScale_ = true;
};

} // namespace ortxui

#endif /* ORTX_UI_DOTMETER_HPP */
