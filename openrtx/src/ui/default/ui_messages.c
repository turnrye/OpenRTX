/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include "ui/ui_default.h"
#include "ui/ui_strings.h"
#include "core/messages.h"
#include "core/graphics.h"
#include "core/input.h"

/* Forward declarations of layout globals defined in ui.c */
extern layout_t layout;
extern state_t last_state;

/**
 * @brief Draw the message inbox list view.
 *
 * Shows up to one screen's worth of inbox entries (newest first).
 * The highlighted entry is indicated by a filled white rectangle.
 * An unread indicator "*" is shown to the left of unread entries.
 * If the inbox is empty, "No messages" is displayed.
 *
 * The "New" affordance appears in the bottom bar when at least one
 * compose-capable source is registered.
 *
 * @param ui_state: pointer to current UI state; uses menu_selected as the
 *                  highlighted row index.
 */
void _ui_drawMessagesList(ui_state_t *ui_state)
{
    gfx_clearScreen();

    /* Title bar */
    gfx_print(layout.top_pos, layout.top_font, TEXT_ALIGN_CENTER, color_white,
              currentLanguage->messages);

    size_t total = messages_count();
    bool can_compose = messages_can_compose(last_state.channel.mode);

    if (total == 0 && !can_compose) {
        point_t center = { CONFIG_SCREEN_WIDTH / 2, CONFIG_SCREEN_HEIGHT / 2 };
        gfx_print(center, layout.menu_font, TEXT_ALIGN_CENTER, color_white,
                  currentLanguage->noMessages);
        return;
    }

    point_t pos = layout.line1_pos;
    uint8_t entries_in_screen =
        (CONFIG_SCREEN_HEIGHT - 1 - pos.y) / layout.menu_h + 1;
    uint8_t selected = ui_state->menu_selected;
    uint8_t scroll = 0;

    if (selected >= entries_in_screen)
        scroll = selected - entries_in_screen + 1;

    for (uint8_t row = 0; pos.y < CONFIG_SCREEN_HEIGHT; row++) {
        size_t idx = row + scroll;
        if (idx >= total)
            break;

        message_header_t *hdr = messages_get(idx);
        if (hdr == NULL)
            break;

        color_t text_color = color_white;
        if (idx == selected) {
            text_color = color_black;
            point_t rect_pos = { 0, pos.y - layout.menu_h + 3 };
            gfx_drawRect(rect_pos, CONFIG_SCREEN_WIDTH, layout.menu_h,
                         color_white, true);
        }

        {
            char line[MAX_ENTRY_LEN] = { 0 };
            /* Unread indicator + other party + truncated body.
             * For TX show recipient; for RX show sender.
             * Format: "U CCCCCC BBBBBBBB"
             * 1 + 6 + 1 + 8 = 16 chars; pixel width at 8pt fits 160px. */
            const char *other = (hdr->direction == MSG_DIR_TX) ?
                                    hdr->recipient :
                                    hdr->sender;
            const char *body_text = (hdr->body != NULL) ?
                                        (const char *)hdr->body :
                                        "";
            sniprintf(line, sizeof(line), "%s%-6.6s %.8s",
                      hdr->unread ? "*" : " ", other, body_text);
            gfx_print(pos, layout.menu_font, TEXT_ALIGN_LEFT, text_color, line);
        }
        pos.y += layout.menu_h;
    }

    /* Virtual "New Message" item at the end of the list */
    if (can_compose && pos.y < CONFIG_SCREEN_HEIGHT) {
        color_t text_color = color_white;
        if ((size_t)selected >= total) {
            text_color = color_black;
            point_t rect_pos = { 0, pos.y - layout.menu_h + 3 };
            gfx_drawRect(rect_pos, CONFIG_SCREEN_WIDTH, layout.menu_h,
                         color_white, true);
        }
        gfx_print(pos, layout.menu_font, TEXT_ALIGN_CENTER, text_color,
                  currentLanguage->newMessage);
        pos.y += layout.menu_h;
    }
}

/**
 * @brief Draw the message detail view.
 *
 * Renders the header (sender + status line) then dispatches the body
 * rendering to the source's vtable->render_detail callback.
 *
 * "Reply" is shown in the bottom bar if MSG_ACTION_REPLY is supported.
 *
 * @param ui_state: pointer to current UI state; menu_selected is the
 *                  snapshot index of the message being displayed.
 */
void _ui_drawMessagesDetail(ui_state_t *ui_state)
{
#ifdef CONFIG_M17_SMS
    gfx_clearScreen();

    size_t idx = ui_state->menu_selected;
    message_header_t *hdr = messages_get(idx);
    if (hdr == NULL) {
        /* Entry disappeared (evicted between ticks): return to list. */
        state.ui_screen = MESSAGES_LIST;
        return;
    }

    /* ---- Inverted header bar: "SENDER > RECIPIENT" ---- */
    /* Clamp scroll each frame before drawing. */
    if (ui_state->detail_scroll > ui_state->detail_scroll_max)
        ui_state->detail_scroll = ui_state->detail_scroll_max;
    if (ui_state->detail_scroll < 0)
        ui_state->detail_scroll = 0;

    /* Full-width filled bar at the top, same height as the standard top
     * bar.  Baseline sits at layout.top_pos.y so the font aligns with
     * every other screen's title. */
    point_t hdr_rect = { 0, 0 };
    gfx_drawRect(hdr_rect, CONFIG_SCREEN_WIDTH, layout.top_h, color_white,
                 true);

    /* "sender > recipient" (or "ALL" when recipient is empty broadcast) */
    const char *sender = hdr->sender;
    const char *recipient = (hdr->recipient[0] != '\0') ? hdr->recipient :
                                                          "ALL";
    char title[32] = { 0 };
    sniprintf(title, sizeof(title), "%.9s > %.9s", sender, recipient);
    gfx_print(layout.top_pos, layout.top_font, TEXT_ALIGN_CENTER, color_black,
              title);

    /* ---- Scrollable body ---- */
    int16_t clip_top = (int16_t)layout.top_h;
    int16_t clip_bot = (int16_t)(CONFIG_SCREEN_HEIGHT - layout.bottom_h - 1);
    uint8_t font_h = gfx_getFontHeight(layout.menu_font);

    const char *body_text = (hdr->body != NULL) ? (const char *)hdr->body : "";

    /* Measure total text height to compute scroll max. */
    uint16_t text_h =
        gfx_measureText(layout.menu_font, body_text, layout.horizontal_pad,
                        CONFIG_SCREEN_WIDTH - layout.horizontal_pad, SIZE_MAX);
    int16_t visible_h = clip_bot - clip_top;
    ui_state->detail_scroll_max = (text_h > (uint16_t)visible_h) ?
                                      (int16_t)(text_h - visible_h) :
                                      0;

    /* Re-clamp after update */
    if (ui_state->detail_scroll > ui_state->detail_scroll_max)
        ui_state->detail_scroll = ui_state->detail_scroll_max;

    point_t text_start = { (int16_t)layout.horizontal_pad,
                           (int16_t)(clip_top + font_h
                                     - ui_state->detail_scroll) };
    gfx_printBufferClipped(text_start, layout.menu_font, TEXT_ALIGN_LEFT,
                           color_white, body_text,
                           CONFIG_SCREEN_WIDTH - layout.horizontal_pad,
                           clip_top, clip_bot);

    /* ---- Inverted bottom bar: "Reply" left, "N/M" centre ---- */
    point_t bot_rect = { 0, (int16_t)(CONFIG_SCREEN_HEIGHT - layout.bottom_h) };
    gfx_drawRect(bot_rect, CONFIG_SCREEN_WIDTH, layout.bottom_h, color_white,
                 true);

    point_t bot_pos = { layout.bottom_pos.x,
                        CONFIG_SCREEN_HEIGHT - layout.bottom_h / 2 };
    if (messages_source_mode(idx) == last_state.channel.mode)
        gfx_print(bot_pos, layout.top_font, TEXT_ALIGN_LEFT, color_black,
                  currentLanguage->reply);

    char counter[12] = { 0 };
    sniprintf(counter, sizeof(counter), "%u/%u", (unsigned)(idx + 1),
              (unsigned)messages_count());
    gfx_print(bot_pos, layout.top_font, TEXT_ALIGN_CENTER, color_black,
              counter);
#endif /* CONFIG_M17_SMS */
}

/**
 * @brief Handle a compose-source picker for the "New" affordance.
 *
 * If exactly one compose-capable source is registered, open it directly.
 * If more than one, display a simple numbered list and route key presses.
 * This function is called from the FSM when the user triggers "New".
 *
 * @param ui_state: pointer to current UI state.
 * @param msg: the key event that triggered the compose flow.
 * @return true if a compose source was launched.
 */
bool _ui_messagesStartCompose(ui_state_t *ui_state, kbd_msg_t msg)
{
    (void)ui_state;
    (void)msg;
    return messages_start_compose(last_state.channel.mode) == 0;
}

/**
 * @brief Draw the message compose form.
 *
 * Layout: "To" label row at top, large scrollable body text area in the
 * middle, "Send" row pinned at the bottom.  The focused row is
 * highlighted (filled white, text in black).  When compose_editing is
 * true a centred bordered overlay is drawn to accept callsign input for
 * the To field.
 *
 * compose_focus: 0 = To row, 1 = body area, 2 = Send row.
 * While focus == 1 character keys type into compose_body directly;
 * UP/DOWN navigate focus (confirming any pending multi-tap char first),
 * LEFT/RIGHT delete the last character.
 *
 * @param ui_state: pointer to current UI state.
 */
void _ui_drawMessagesCompose(ui_state_t *ui_state)
{
#ifdef CONFIG_M17_SMS
    gfx_clearScreen();

    /* Title bar */
    const char *title = ui_state->compose_is_reply ?
                            currentLanguage->reply :
                            currentLanguage->newMessage;
    gfx_print(layout.top_pos, layout.top_font, TEXT_ALIGN_CENTER, color_white,
              title);

    /* ---- To row (row 0) ---- */
    point_t to_rpos = layout.line1_pos;

    if (!ui_state->compose_editing && ui_state->compose_focus == 0) {
        point_t rect_pos = { 0, (int16_t)(to_rpos.y - layout.menu_h + 3) };
        gfx_drawRect(rect_pos, CONFIG_SCREEN_WIDTH, layout.menu_h, color_white,
                     true);
    }

    color_t to_col = (ui_state->compose_focus == 0
                      && !ui_state->compose_editing) ?
                         color_black :
                         color_white;
    {
        const char *rcpt;
        if (ui_state->compose_recipient[0] != '\0')
            rcpt = ui_state->compose_recipient;
        else if (last_state.settings.m17_dest[0] != '\0')
            rcpt = last_state.settings.m17_dest;
        else
            rcpt = currentLanguage->broadcast;
        char val[11] = { 0 };
        sniprintf(val, sizeof(val), "%.10s", rcpt);
        gfx_print(to_rpos, layout.menu_font, TEXT_ALIGN_LEFT, to_col, "To");
        gfx_print(to_rpos, layout.menu_font, TEXT_ALIGN_RIGHT, to_col, val);
    }

    /* ---- Body area (row 1) ---- */
    /* Spans from just below the To-row highlight to just above the
     * Send-row highlight, with a 2 px inner padding on all sides.
     * The Send row is pinned to the screen bottom (layout.bottom_pos),
     * not to the generic bottom bar, so the full screen height is used. */
    static const uint8_t BOX_PAD = 2;
    int16_t to_row_top = (int16_t)(to_rpos.y - layout.menu_h + 3);
    int16_t body_top = (int16_t)(to_row_top + layout.menu_h);
    /* send_h_top: pixel row where the Send highlight rect begins.
     * layout.bottom_pos.y is the text baseline at the very bottom of
     * the screen (identical to what other screens use for their bottom
     * bar).  Subtract (menu_h - 3) to get the rect's top edge so the
     * baseline lands at layout.bottom_pos.y. */
    int16_t send_h_top = (int16_t)(layout.bottom_pos.y - layout.menu_h + 3);
    int16_t body_bot = (int16_t)(send_h_top - 1);
    uint16_t body_w =
        (uint16_t)(CONFIG_SCREEN_WIDTH - 2u * layout.horizontal_pad);
    uint16_t body_h = (uint16_t)(body_bot - body_top + 1);
    point_t body_orig = { (int16_t)layout.horizontal_pad, body_top };

    /* Fill only when actively typing into the body (editing mode).
     * When merely focused (not yet pressed ENTER), show outline only —
     * this signals to the user that ENTER activates text entry. */
    if (ui_state->compose_focus == 1 && ui_state->compose_body_editing)
        gfx_drawRect(body_orig, body_w, body_h, color_white, true);
    gfx_drawRect(body_orig, body_w, body_h, color_white, false);

    color_t body_col = (ui_state->compose_focus == 1
                        && ui_state->compose_body_editing) ?
                           color_black :
                           color_white;
    uint8_t font_h = gfx_getFontHeight(layout.message_font);
    uint16_t max_x = (uint16_t)(layout.horizontal_pad + body_w - BOX_PAD);
    int16_t clip_top = (int16_t)(body_top + 1);
    int16_t clip_bot = (int16_t)(body_bot - 1);

    uint16_t cursor_y =
        gfx_measureText(layout.message_font, ui_state->compose_body,
                        (uint16_t)(layout.horizontal_pad + BOX_PAD), max_x,
                        (size_t)ui_state->input_position + 1);
    int16_t visible_h = clip_bot - clip_top;
    int16_t scroll_offset = 0;
    if ((int16_t)cursor_y > visible_h)
        scroll_offset = (int16_t)cursor_y - visible_h;

    point_t text_start = { (int16_t)(layout.horizontal_pad + BOX_PAD),
                           (int16_t)(clip_top + font_h - scroll_offset) };
    gfx_printBufferClipped(text_start, layout.message_font, TEXT_ALIGN_LEFT,
                           body_col, ui_state->compose_body, max_x, clip_top,
                           clip_bot);

    /* ---- Send row (row 2) ---- */
    /* Pinned to the very bottom of the screen, using the same baseline
     * position (layout.bottom_pos.y) as all other screens' bottom bars. */
    point_t send_rpos = { (int16_t)layout.horizontal_pad, layout.bottom_pos.y };
    if (ui_state->compose_focus == 2) {
        point_t rect_pos = { 0, (int16_t)(send_rpos.y - layout.menu_h + 3) };
        gfx_drawRect(rect_pos, CONFIG_SCREEN_WIDTH, layout.menu_h, color_white,
                     true);
    }
    color_t send_col = (ui_state->compose_focus == 2) ? color_black :
                                                        color_white;
    gfx_print(send_rpos, layout.menu_font, TEXT_ALIGN_CENTER, send_col,
              currentLanguage->send);

    /* ---- To-field editing overlay ---- */
    if (ui_state->compose_editing) {
        uint16_t rect_width = CONFIG_SCREEN_WIDTH - (layout.horizontal_pad * 2);
        uint16_t rect_height =
            (CONFIG_SCREEN_HEIGHT - (layout.top_h + layout.bottom_h)) / 2;
        point_t rect_origin = {
            (int16_t)((CONFIG_SCREEN_WIDTH - rect_width) / 2),
            (int16_t)((CONFIG_SCREEN_HEIGHT - rect_height) / 2)
        };
        /* Solid black fill first to prevent row text showing through */
        gfx_drawRect(rect_origin, rect_width, rect_height, color_black, true);
        gfx_drawRect(rect_origin, rect_width, rect_height, color_white, false);
        gfx_printLine(1, 1, layout.top_h,
                      CONFIG_SCREEN_HEIGHT - layout.bottom_h,
                      layout.horizontal_pad, layout.input_font,
                      TEXT_ALIGN_CENTER, color_white, ui_state->new_callsign);
    }
#endif /* CONFIG_M17_SMS */
}
