/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ui/T9InputControl.h"
#include "ui/ui_default.h"
#include "core/input.h"
#include "core/voicePromptUtils.h"
#include "core/graphics.h"
#include "interfaces/delays.h"
#include "hwconfig.h"
#include <cstring>

extern const color_t color_white;

const char* const m17CallsignSymbols[] =
{
    "0 ",
    "1",
    "ABC2",
    "DEF3",
    "GHI4",
    "JKL5",
    "MNO6",
    "PQRS7",
    "TUV8",
    "WXYZ9",
    "-/",
    ""
};

const char* const t9TextSymbols[] =
{
    " 0",
    ",.?1",
    "abc2ABC",
    "def3DEF",
    "ghi4GHI",
    "jkl5JKL",
    "mno6MNO",
    "pqrs7PQRS",
    "tuv8TUV",
    "wxyz9WXYZ",
    "-/*",
    "#"
};

void T9InputControl::start(char* buf, uint8_t maxLen,
                           const char* const* symbols, fontSize_t font)
{
    buf_            = buf;
    maxLen_         = maxLen;
    symbols_        = symbols;
    font_           = font;
    input_position_ = 0;
    input_set_      = 0;
    input_number_   = 0;
    last_keypress_  = 0;
    memset(buf_, 0, maxLen_ + 1);
    buf_[0] = '_';
}

void T9InputControl::reset()
{
    input_position_ = 0;
    input_set_      = 0;
    input_number_   = 0;
    last_keypress_  = 0;
}

InputResult T9InputControl::handleKey(UIContext& ctx, event_t event)
{
    (void)ctx;

    kbd_msg_t msg;
    msg.value = event.payload;

    if(msg.keys & KEY_ENTER)
    {
        confirm();
        return InputResult::Confirmed;
    }

    if(msg.keys & KEY_ESC)
        return InputResult::Cancelled;

    if(msg.keys & KEY_UP || msg.keys & KEY_DOWN ||
       msg.keys & KEY_LEFT || msg.keys & KEY_RIGHT)
    {
        handleDelete();
    }
    else if(input_isCharPressed(msg))
    {
        long long now   = getTick();
        uint8_t num_key = input_getPressedChar(msg);

        bool key_timeout = ((now - last_keypress_) >= input_longPressTimeout);
        bool same_key    = input_number_ == num_key;

        uint8_t num_symbols = strlen(symbols_[num_key]);
        if(num_symbols == 0)
            return InputResult::Ongoing;

        if((input_position_ >= maxLen_) ||
           ((input_position_ == (maxLen_ - 1)) && (key_timeout || !same_key)))
            return InputResult::Ongoing;

        if(last_keypress_ != 0)
        {
            if(same_key && !key_timeout)
            {
                input_set_ = (input_set_ + 1) % num_symbols;
            }
            else
            {
                input_position_++;
                input_set_ = 0;
            }
        }

        buf_[input_position_] = symbols_[num_key][input_set_];
        vp_announceInputChar(buf_[input_position_]);
        input_number_  = num_key;
        last_keypress_ = now;
    }

    return InputResult::Ongoing;
}

void T9InputControl::confirm()
{
    if(!buf_)
        return;

    if(buf_[input_position_] == '_')
        buf_[input_position_] = '\0';
    else
        buf_[input_position_ + 1] = '\0';
}

void T9InputControl::handleDelete()
{
    if(!buf_)
        return;

    if(buf_[input_position_] && buf_[input_position_] != '_')
        vp_announceInputChar(buf_[input_position_]);

    buf_[input_position_] = '\0';

    if(input_position_ > 0)
    {
        input_position_--;
    }
    else
    {
        last_keypress_ = 0;
    }

    input_set_ = 0;
}

void T9InputControl::draw(UIContext& ctx, bool overlay)
{
    if(!buf_)
        return;

    if(overlay)
    {
        uint16_t rect_width  = CONFIG_SCREEN_WIDTH -
                               (ctx.layout.horizontal_pad * 2);
        uint16_t rect_height = (CONFIG_SCREEN_HEIGHT -
                               (ctx.layout.top_h + ctx.layout.bottom_h)) / 2;
        point_t rect_origin  = {(int16_t)((CONFIG_SCREEN_WIDTH - rect_width) / 2),
                                (int16_t)((CONFIG_SCREEN_HEIGHT - rect_height) / 2)};
        gfx_drawRect(rect_origin, rect_width, rect_height, color_white, false);
    }

    gfx_printLine(1, 1, ctx.layout.top_h,
                  CONFIG_SCREEN_HEIGHT - ctx.layout.bottom_h,
                  ctx.layout.horizontal_pad, font_,
                  TEXT_ALIGN_CENTER, color_white, buf_);
}
