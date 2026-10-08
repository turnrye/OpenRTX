/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/mic_frame.h"
#include "hwconfig.h"
#include <math.h>
#include <string.h>

#ifndef CONFIG_MIC_GAIN
#define CONFIG_MIC_GAIN 1
#endif

#ifndef CONFIG_MIC_OVERSAMPLE
#define CONFIG_MIC_OVERSAMPLE 1
#endif

#ifndef CONFIG_MIC_GATE_OPEN
#define CONFIG_MIC_GATE_OPEN 0
#endif

#ifndef CONFIG_MIC_GATE_CLOSE
#define CONFIG_MIC_GATE_CLOSE 0
#endif

void micFrame_init(struct micFrameState *state)
{
    memset(state, 0, sizeof(*state));
    state->openLevel = CONFIG_MIC_GATE_OPEN;
    state->closeLevel = CONFIG_MIC_GATE_CLOSE;
    state->open = true;
}

size_t micFrame_condition(struct micFrameState *state, int16_t *buf, size_t len)
{
    size_t out = 0;

    for (size_t i = 0; i < len; i++) {
        int16_t sample = buf[i];

#if CONFIG_MIC_OVERSAMPLE > 1
        if (dsp_decimator(&state->decimator, &sample, CONFIG_MIC_OVERSAMPLE)
            == false)
            continue;
#endif

        sample = dsp_dcBlockFilter(&state->dcBlock, sample);
        buf[out++] = sample * CONFIG_MIC_GAIN;
    }

    return out;
}

uint16_t micFrame_rms(const int16_t *frame, size_t len)
{
    if (len == 0)
        return 0;

    uint64_t acc = 0;
    for (size_t i = 0; i < len; i++)
        acc += (int32_t)frame[i] * (int32_t)frame[i];

    return (uint16_t)sqrtf((float)(acc / len));
}

bool micFrame_hasVoice(struct micFrameState *state, const int16_t *frame,
                       size_t len)
{
    if (state->openLevel == 0)
        return true;

    uint16_t rms = micFrame_rms(frame, len);

    if (rms >= state->openLevel) {
        state->open = true;
        state->hold = MIC_GATE_HOLD_FRAMES;
    } else if (state->open && (rms < state->closeLevel)) {
        if (state->hold > 0)
            state->hold -= 1;
        else
            state->open = false;
    }

    return state->open;
}
