/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ui/ArrowInputControl.h"
#include "ui/ui_mod17.h"
#include "core/input.h"
#include "core/graphics.h"
#include "hwconfig.h"
#include <cstring>

extern const color_t color_white;

const char m17CallsignSymbols[] =
    "_ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890/-.";

const char t9TextSymbols[] =
    "_ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890/-.";

void ArrowInputControl::start(char* buf, uint8_t maxLen, const char* symbols,
                              fontSize_t font)
{
    buf_            = buf;
    maxLen_         = maxLen;
    symbols_        = symbols;
    font_           = font;
    input_position_ = 0;
    input_set_      = 0;
    memset(buf_, 0, maxLen_ + 1);
}

void ArrowInputControl::reset()
{
    input_position_ = 0;
    input_set_      = 0;
}

InputResult ArrowInputControl::handleKey(UIContext& ctx, event_t event)
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

    if(msg.keys & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT))
    {
        if(input_position_ >= maxLen_)
            return InputResult::Ongoing;

        uint8_t num_symbols = strlen(symbols_);

        if(msg.keys & KEY_RIGHT)
        {
            if(input_position_ < (maxLen_ - 1))
            {
                input_position_++;
                input_set_ = 0;
            }
        }
        else if(msg.keys & KEY_LEFT)
        {
            if(input_position_ > 0)
            {
                buf_[input_position_] = '\0';
                input_position_--;
            }
            input_set_ = strcspn(symbols_, &buf_[input_position_]);
        }
        else if(msg.keys & KEY_UP)
            input_set_ = (input_set_ + 1) % num_symbols;
        else if(msg.keys & KEY_DOWN)
            input_set_ = input_set_ == 0 ? num_symbols - 1 : input_set_ - 1;

        buf_[input_position_] = symbols_[input_set_];
    }

    return InputResult::Ongoing;
}

void ArrowInputControl::confirm()
{
    if(!buf_)
        return;

    buf_[input_position_ + 1] = '\0';
}

void ArrowInputControl::draw(UIContext& ctx, bool overlay)
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
