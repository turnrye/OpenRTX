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
 * kHz, small trailing sub-kHz digits sharing its baseline, and the mode /
 * bandwidth label (WFM/NFM/M17) right-aligned on that same baseline. Custom-
 * drawn because the mixed-size baseline alignment is finer than the box layout
 * expresses. Secondary M17/FM details (CAN, tone) live in the channel row
 * below, not here. The mode label is a borrowed const string.
 */
class FreqHero : public Object
{
public:
    void setFreq(uint32_t hz);
    /** Right-aligned mode/bandwidth label (WFM/NFM/M17), on the frequency
     *  baseline. Power is intentionally omitted — it is shown on the meter
     *  while transmitting. */
    void setMode(const char *mode)
    {
        mode_ = (mode != nullptr) ? mode : "";
    }

    void draw(DrawCtx &d) override;

private:
    char mainBuf_[12] = "0.000";
    char subBuf_[4] = "00";
    const char *mode_ = "";
};

} // namespace ortxui

#endif /* ORTX_UI_FREQHERO_HPP */
