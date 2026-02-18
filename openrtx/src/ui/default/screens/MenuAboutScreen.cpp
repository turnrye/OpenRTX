/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ui/Screen.h"
#include "ui/UIContext.h"
#include "ui/ui_default.h"
#include "ui/ui_strings.h"
#include "core/input.h"
#include "core/event.h"
#include "core/graphics.h"
#include "core/state.h"
#include "hwconfig.h"

extern const color_t color_white;
extern const color_t yellow_fab413;

static const char* about_authors[] =
{
    "Niccolo' IU2KIN",
    "Silvano IU2KWO",
    "Federico IU2NUO",
    "Fred IU2NRO",
    "Joseph VK7JS",
    "Morgan ON4MOD",
    "Marco DM4RCO"
};
static const uint8_t about_author_num = sizeof(about_authors) / sizeof(about_authors[0]);

class MenuAboutScreen : public Screen
{
public:
    bool handleInput(UIContext& ctx, event_t event, bool* sync_rtx) override
    {
        (void) sync_rtx;
        kbd_msg_t msg;
        msg.value = event.payload;

        if (msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
        {
            if (ctx.ui_state.menu_selected > 0)
                ctx.ui_state.menu_selected -= 1;
        }
        else if (msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
        {
            ctx.ui_state.menu_selected += 1;
        }
        else if (msg.keys & KEY_ESC)
        {
            if (ctx.ui_state.edit_mode)
            {
                ctx.ui_state.edit_mode = false;
            }
            else
            {
                state.ui_screen = MENU_TOP;
                ctx.ui_state.menu_selected = 0;
            }
        }

        return true;
    }

    void draw(UIContext& ctx) override
    {
        gfx_clearScreen();

        point_t logo_pos;
        if (CONFIG_SCREEN_HEIGHT >= 100)
        {
            logo_pos.x = 0;
            logo_pos.y = CONFIG_SCREEN_HEIGHT / 5;
            gfx_print(logo_pos, FONT_SIZE_12PT, TEXT_ALIGN_CENTER,
                      yellow_fab413, "O P N\nR T X");
        }
        else
        {
            logo_pos.x = ctx.layout.horizontal_pad;
            logo_pos.y = ctx.layout.line3_large_h;
            gfx_print(logo_pos, ctx.layout.line3_large_font, TEXT_ALIGN_CENTER,
                      yellow_fab413, currentLanguage->openRTX);
        }

        point_t pos;
        pos.x = CONFIG_SCREEN_WIDTH / 7;
        pos.y = CONFIG_SCREEN_HEIGHT - (ctx.layout.menu_h * 3) - 5;

        uint8_t entries_in_screen = (CONFIG_SCREEN_HEIGHT - 1 - pos.y) / ctx.layout.menu_h + 1;
        uint8_t max_scroll = about_author_num - entries_in_screen;

        if (ctx.ui_state.menu_selected >= max_scroll)
            ctx.ui_state.menu_selected = max_scroll;

        for (uint8_t item = 0; item < entries_in_screen; item++)
        {
            uint8_t elem = ctx.ui_state.menu_selected + item;
            gfx_print(pos, ctx.layout.menu_font, TEXT_ALIGN_LEFT,
                      color_white, about_authors[elem]);
            pos.y += ctx.layout.menu_h;
        }
    }
};

static MenuAboutScreen menuAboutInstance;

Screen* getMenuAboutScreen()
{
    return &menuAboutInstance;
}
