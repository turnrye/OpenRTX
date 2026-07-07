/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_FREQHERO_HPP
#define ORTX_UI_FREQHERO_HPP

#include <cstdint>
#include "core/Object.hpp"

namespace ortxui
{

/**
 * The VFO frequency hero (mockups 5-6): a large left-aligned frequency to
 * kHz, small trailing sub-kHz digits sharing its baseline, and a right-aligned
 * mode stack (mode/bandwidth over the PL tone). Custom-drawn because the mixed-
 * size baseline alignment is finer than the box layout expresses. The mode
 * lines are borrowed const strings (literals chosen by the view).
 */
class FreqHero : public Object
{
public:
    void setFreq(uint32_t hz);
    /** Right-hand mode stack: mode/bandwidth (m1) over the PL tone (m2). Power
     *  is intentionally omitted — it is shown on the meter while transmitting. */
    void setMode(const char *m1, const char *m2)
    {
        m1_ = (m1 != nullptr) ? m1 : "";
        m2_ = (m2 != nullptr) ? m2 : "";
    }

    void draw(DrawCtx &d) override;

private:
    char mainBuf_[12] = "0.000";
    char subBuf_[4] = "00";
    const char *m1_ = "";
    const char *m2_ = "";
};

} // namespace ortxui

#endif /* ORTX_UI_FREQHERO_HPP */
