/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ui/TextInputControl.h"
#include "core/input.h"
#include "interfaces/delays.h"
#include "core/graphics.h"
#include "hwconfig.h"
#include <cstring>

#ifdef CONFIG_UI_MODULE17
#include "ui/ui_mod17.h"
#else
#include "ui/ui_default.h"
#include "core/voicePromptUtils.h"
#endif

extern const color_t color_white;

#ifdef CONFIG_UI_MODULE17

const SymbolTable m17CallsignSymbols =
    "_ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890/-.";

#else

static const char* const m17CallsignSymbolsArr[] =
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

const SymbolTable m17CallsignSymbols = m17CallsignSymbolsArr;

#endif

void TextInputControl::start(char* buf, uint8_t maxLen, SymbolTable symbols)
{
    buf_     = buf;
    maxLen_  = maxLen;
    symbols_ = symbols;
    input_position_ = 0;
    input_set_      = 0;
    input_number_   = 0;
    last_keypress_  = 0;
    memset(buf_, 0, maxLen_ + 1);
    buf_[0] = '_';
}

void TextInputControl::reset()
{
    input_position_ = 0;
    input_set_      = 0;
    input_number_   = 0;
    last_keypress_  = 0;
}

InputResult TextInputControl::handleKey(UIContext& ctx, event_t event)
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

#ifdef CONFIG_UI_MODULE17
    // Arrow-key character selection
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
#else
    // Default variant: arrow keys = delete
    if(msg.keys & KEY_UP || msg.keys & KEY_DOWN ||
       msg.keys & KEY_LEFT || msg.keys & KEY_RIGHT)
    {
        handleDelete();
    }
    else if(input_isCharPressed(msg))
    {
        // T9 multi-tap keypad input
        long long now = getTick();
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
        input_number_   = num_key;
        last_keypress_  = now;
    }
#endif

    return InputResult::Ongoing;
}

void TextInputControl::confirm()
{
    if(buf_)
        buf_[input_position_ + 1] = '\0';
}

void TextInputControl::handleDelete()
{
    if(!buf_)
        return;

#ifndef CONFIG_UI_MODULE17
    if(buf_[input_position_] && buf_[input_position_] != '_')
        vp_announceInputChar(buf_[input_position_]);
#endif

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

void TextInputControl::draw(UIContext& ctx, bool overlay)
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
                  ctx.layout.horizontal_pad, ctx.layout.input_font,
                  TEXT_ALIGN_CENTER, color_white, buf_);
}
