/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/cps.h"
#include "core/tuner.h"
#include "interfaces/platform.h"
#include "interfaces/cps_io.h"
#include "interfaces/delays.h"
#include "rtx/rtx.h"
#include <stdio.h>
#include <stdint.h>
#include "ui/ui_default.h"
#include <string.h>
#include "ui/ui_strings.h"
#include "core/utils.h"
#include "ui/utils.h"

void _ui_drawMainBackground()
{
    // Print top bar line of hline_h pixel height
    gfx_drawHLine(layout.top_h, layout.hline_h, color_grey);
    // Print bottom bar line of 1 pixel height
    gfx_drawHLine(CONFIG_SCREEN_HEIGHT - layout.bottom_h - 1, layout.hline_h, color_grey);
}

void _ui_drawMainTop(ui_state_t * ui_state)
{
#ifdef CONFIG_RTC
    // Print clock on top bar
    datetime_t local_time = utcToLocalTime(last_state.time,
                                           last_state.settings.utc_timezone);
    gfx_print(layout.top_pos, layout.top_font, TEXT_ALIGN_CENTER,
              color_white, "%02d:%02d:%02d", local_time.hour,
              local_time.minute, local_time.second);
#endif
    // If the radio has no built-in battery, print input voltage
#ifdef CONFIG_BAT_NONE
    gfx_print(layout.top_pos, layout.top_font, TEXT_ALIGN_RIGHT,
              color_white,"%u.%uV", last_state.v_bat/1000, (last_state.v_bat % 1000) /100);
#else
    if(last_state.settings.showBatteryIcon) {
        // print battery icon on top bar, use 4 px padding
        uint16_t bat_width = CONFIG_SCREEN_WIDTH / 9;
        uint16_t bat_height = layout.top_h - (layout.status_v_pad * 2);
        point_t bat_pos = {CONFIG_SCREEN_WIDTH - bat_width - layout.horizontal_pad,
                        layout.status_v_pad};
        gfx_drawBattery(bat_pos, bat_width, bat_height, last_state.charge);
    } else {
        // print the battery percentage
        point_t bat_pos = {layout.top_pos.x, layout.top_pos.y - 2};
        gfx_print(bat_pos , FONT_SIZE_6PT, TEXT_ALIGN_RIGHT,
        color_white,"%d%%", last_state.charge);
    }
#endif
    if (ui_state->input_locked == true)
      gfx_drawSymbol(layout.top_pos, layout.top_symbol_size, TEXT_ALIGN_LEFT,
                     color_white, SYMBOL_LOCK);
}

void _ui_drawBankChannel()
{
    // Print Bank number, channel number and Channel name
    uint16_t b = (tuner_get_mode(&last_state.tuner) == TUNER_BANK) ? last_state.tuner.core.bank_index : 0;
    char name[32];
    tuner_get_channel_name(&last_state.tuner, name, sizeof(name));
    gfx_print(layout.line1_pos, layout.line1_font, TEXT_ALIGN_CENTER,
              color_white, "%01d-%03d: %.12s",
              b, last_state.channel_index + 1, name);
}

const char* _ui_getToneEnabledString(bool tone_tx_enable, bool tone_rx_enable,
        bool use_abbreviation)
{
    const char *strings[2][4] = {
        {
            currentLanguage->None,
            currentLanguage->Encode,
            currentLanguage->Decode,
            currentLanguage->Both,
        }, {
            "N",
            "T",
            "R",
            "B"
        }
    };


    uint8_t index = (tone_rx_enable << 1) | (tone_tx_enable);
    return strings[use_abbreviation][index];
}

void _ui_drawModeInfo(ui_state_t* ui_state)
{
    char bw_str[8] = { 0 };

    switch(tuner_get_opmode(&last_state.tuner))
    {
        case OPMODE_FM:
        {
            fmInfo_t fm;
            tuner_get_FM_params(&last_state.tuner, &fm);

            // Get Bandwidth string
            if(tuner_get_bandwidth(&last_state.tuner) == BW_12_5)
                sniprintf(bw_str, 8, "NFM");
            else if(tuner_get_bandwidth(&last_state.tuner) == BW_25)
                sniprintf(bw_str, 8, "FM");

            // Get encdec string
            bool tone_tx_enable = fm.txToneEn;
            bool tone_rx_enable = fm.rxToneEn;
            // Print Bandwidth, Tone and encdec info
            if (tone_tx_enable || tone_rx_enable)
            {
                uint16_t tone = ctcss_tone[fm.txTone];
                gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                          color_white, "%s %d.%d %s", bw_str, (tone / 10),
                          (tone % 10), _ui_getToneEnabledString(tone_tx_enable, tone_rx_enable, true));
            }
            else
            {
                gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                          color_white, "%s", bw_str );
            }
        }
            break;

        case OPMODE_DMR:
            // Print talkgroup
            gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                    color_white, "DMR TG%s", "");
            break;

        #ifdef CONFIG_M17
        case OPMODE_M17:
        {
            // Print M17 Destination ID on line 3 of 3
            rtxStatus_t rtxStatus = rtx_getCurrentStatus();

            if(rtxStatus.lsfOk)
            {
                // Destination address
                gfx_drawSymbol(layout.line2_pos, layout.line2_symbol_size, TEXT_ALIGN_LEFT,
                               color_white, SYMBOL_CALL_RECEIVED);

                gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                          color_white, "%s", rtxStatus.M17_dst);

                // Source address
                gfx_drawSymbol(layout.line1_pos, layout.line1_symbol_size, TEXT_ALIGN_LEFT,
                               color_white, SYMBOL_CALL_MADE);

                gfx_print(layout.line1_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                          color_white, "%s", rtxStatus.M17_src);

                // RF link (if present)
                if(rtxStatus.M17_link[0] != '\0')
                {
                    gfx_drawSymbol(layout.line4_pos, layout.line3_symbol_size, TEXT_ALIGN_LEFT,
                                   color_white, SYMBOL_ACCESS_POINT);

                    gfx_print(layout.line4_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                              color_white, "%s", rtxStatus.M17_link);
                }

                // Meta text (if present)
                if(rtxStatus.M17_meta_text[0] != '\0')
                {
                    char msg[14];
                    
                    // Always display current position
                    scrollTextPeek(rtxStatus.M17_meta_text, msg, sizeof(msg),
                                   ui_state->m17_meta_text_scroll_position);
                    // Only advance scroll every 100ms
                    long long now = getTick();
                    if(now - ui_state->m17_meta_text_last_scroll_tick >= 100)
                    {
                        scrollTextAdvance(rtxStatus.M17_meta_text, sizeof(msg),
                                          &ui_state->m17_meta_text_scroll_position);
                        ui_state->m17_meta_text_last_scroll_tick = now;
                    }
                    gfx_print(layout.line3_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                              color_white, "%s", msg);
                }
                // Reflector (if present)
                else if(rtxStatus.M17_refl[0] != '\0')
                {                    
                    gfx_drawSymbol(layout.line3_pos, layout.line4_symbol_size, TEXT_ALIGN_LEFT,
                                   color_white, SYMBOL_NETWORK);

                    gfx_print(layout.line3_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                              color_white, "%s", rtxStatus.M17_refl);
                }
                
                // Reset scroll position when meta text becomes empty
                if(rtxStatus.M17_meta_text[0] == '\0')
                {
                    ui_state->m17_meta_text_scroll_position = 0;
                }
            }
            else
            {
                const char *dst = NULL;
                if(ui_state->edit_mode)
                {
                    dst = ui_state->new_callsign;
                }
                else
                {
                    if(strnlen(rtxStatus.destination_address, 10) == 0)
                        dst = currentLanguage->broadcast;
                    else
                        dst = rtxStatus.destination_address;
                }

                gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                          color_white, "M17 #%s", dst);
            }
            break;
        }
        #endif
    }
}

// Color of the band that is transmitting
static const color_t color_tx = { 255, 0, 0, 255 };

/**
 * Print a frequency in MHz with its last two digits in a smaller font,
 * centred inside a horizontal span.
 *
 * @param freq: frequency, in Hz.
 * @param x: left end of the span.
 * @param width: width of the span.
 * @param y: vertical position of the text line.
 * @param font: font of the main digits.
 * @param color: text color.
 */
static void _ui_printFrequency(freq_t freq, uint16_t x, uint16_t width,
                               uint16_t y, fontSize_t font, color_t color)
{
    char freq_str[16] = {0};
    sniprintf(freq_str, sizeof(freq_str), "%03lu.%05lu",
              (freq / 1000000lu), (freq % 1000000lu) / 10);

    size_t len = strlen(freq_str);
    char main_str[16] = {0};
    char small_str[3] = {0};
    strncpy(main_str, freq_str, len - 2);
    strncpy(small_str, freq_str + len - 2, 2);

    fontSize_t small_font = FONT_SIZE_5PT;
    if (font > FONT_SIZE_6PT)
    {
        small_font = (fontSize_t)(font - 2);
    }

    uint16_t main_width = gfx_getTextWidth(font, main_str);
    uint16_t small_width = gfx_getTextWidth(small_font, small_str);
    uint16_t total_width = main_width + small_width;
    int16_t start_x = x + (width - total_width) / 2;

    point_t main_pos = { (uint16_t)start_x, y };
    gfx_print(main_pos, font, TEXT_ALIGN_LEFT, color, "%s", main_str);

    point_t small_pos = { (uint16_t)(start_x + main_width), y };
    gfx_print(small_pos, small_font, TEXT_ALIGN_LEFT, color, "%s", small_str);
}

void _ui_drawFrequency()
{
    freq_t freq = platform_getPttStatus() ? tuner_get_tx_frequency(&last_state.tuner)
                                          : tuner_get_rx_frequency(&last_state.tuner);

    _ui_printFrequency(freq, 0, CONFIG_SCREEN_WIDTH, layout.line3_large_pos.y,
                       layout.line3_large_font, color_white);
}

/**
 * Dual VFO: draw one of the two bands. VFO A is always the upper band and
 * VFO B the lower one, the selected one carries the MAIN tag. Each band has
 * its own mode, frequency, BUSY flag and signal meter.
 *
 * Screens taller than 64 pixels have a status row above the frequency. The
 * others have room for one row only: letter, frequency and mode. There the
 * letter of the selected band is reversed in place of the tag, and the BUSY
 * flag takes the place of the mode.
 *
 * While transmitting, the selected band shows TX in place of the MAIN tag,
 * or of the mode with one row, and turns red on color screens.
 *
 * @param vfo: band to draw, VFO_A or VFO_B.
 * @param top: vertical position of the top of the band.
 * @param height: height of the band.
 * @param status: current RTX status.
 */
static void _ui_drawVfoBand(uint8_t vfo, uint16_t top, uint16_t height,
                            const rtxStatus_t *status)
{
    const bool compact = (CONFIG_SCREEN_HEIGHT <= 64);
    bool selected = (vfo == last_state.settings.vfoSelected);
    const struct tuner *t = selected ? &last_state.tuner
                                     : &last_state.tuner_alt;
    const uint8_t opmode = tuner_get_opmode(t);
    fmInfo_t fm = { 0 };
    tuner_get_FM_params(t, &fm);
    rssi_t rssi = selected ? last_state.rssi : last_state.rssi_alt;

    // The receiver is on the selected VFO unless it sits on the watched one
    bool tuned = (selected != (status->onWatch == 1));
    bool busy = tuned && rtx_rxSquelchOpen();
    bool tx = selected && (status->opStatus == TX);
    color_t text = tx ? color_tx : color_white;

    // Signal meter at the bottom of the band: S0 to S9+20 over a base line,
    // with a mark at the squelch level
    uint16_t meter_h = compact ? 3 : 5;
    uint16_t width = CONFIG_SCREEN_WIDTH - 2 * layout.horizontal_pad;
    uint16_t level = (rssiToSlevel(rssi) * width) / 11;
    point_t base = { layout.horizontal_pad, (uint16_t)(top + height - 2) };
    point_t bar = { base.x, (uint16_t)(base.y - meter_h) };
    gfx_drawRect(base, width, 1, color_grey, true);
    gfx_drawRect(bar, level, meter_h, busy ? yellow_fab413 : color_white,
                 true);

    if(opmode == OPMODE_FM)
    {
        rssi_t squelch = sqlLevelToRssi(tuner_get_sql_level(t));
        point_t mark = { (uint16_t)(base.x
                                    + (rssiToSlevel(squelch) * width) / 11),
                         (uint16_t)(bar.y - 1) };
        gfx_drawRect(mark, 1, meter_h + 2, yellow_fab413, true);
    }

    // Mode, with the tone on the screens that have a status row
    char mode[24] = "--";
    if(opmode == OPMODE_FM)
    {
        const char *bw = (tuner_get_bandwidth(t) == BW_12_5) ? "NFM" : "FM";
        uint16_t tone = ctcss_tone[fm.txTone];
        const char *dir = _ui_getToneEnabledString(fm.txToneEn,
                                                   fm.rxToneEn, true);
        if((fm.txToneEn == 0) && (fm.rxToneEn == 0))
            sniprintf(mode, sizeof(mode), "%s", bw);
        else if(compact)
            sniprintf(mode, sizeof(mode), "%s %s", bw, dir);
        else
            sniprintf(mode, sizeof(mode), "%s %d.%d %s", bw, tone / 10,
                      tone % 10, dir);
    }
    else if(opmode == OPMODE_M17)
    {
        const char *dst = status->destination_address;
        if(strnlen(dst, 10) == 0)
            dst = currentLanguage->broadcast;

        if(selected && (compact == false))
            sniprintf(mode, sizeof(mode), "M17 #%s", dst);
        else
            sniprintf(mode, sizeof(mode), "M17");
    }
    else if(opmode == OPMODE_DMR)
        sniprintf(mode, sizeof(mode), "DMR");

    // A tuner in memory mode shows its channel name: before the mode on the
    // status row, in place of the frequency on the single row screens
    char name[CPS_STR_SIZE + 1] = { 0 };
    tuner_get_channel_name(t, name, sizeof(name) - 1);
    bool memory = (tuner_get_mode(t) != TUNER_VFO);
    if(memory && (compact == false))
    {
        char tmp[sizeof(mode)];
        sniprintf(tmp, sizeof(tmp), "%.10s %.12s", name, mode);
        memcpy(mode, tmp, sizeof(mode));
    }

    // Text lines: the frequency sits above the meter, the status either on
    // its own row at the top of the band or on the line of the frequency
    fontSize_t font = layout.line1_font;
    uint16_t font_h = gfx_getFontHeight(font);
    uint16_t gap = compact ? 2 : layout.horizontal_pad;
    uint16_t freq_y = bar.y - (compact ? 3 : 6);
    uint16_t text_y = compact ? freq_y : (uint16_t)(top + font_h);
    uint16_t right = CONFIG_SCREEN_WIDTH - layout.horizontal_pad;

    // Letter and MAIN tag. With one row there is no room for the tag: the
    // letter of the selected band is drawn reversed instead.
    point_t pos = { layout.horizontal_pad, text_y };
    const char *letter = (vfo == VFO_B) ? "B" : "A";
    uint16_t letter_w = gfx_getTextWidth(font, "B");
    uint16_t tag_w = gfx_getTextWidth(font, "MAIN") + 2;
    point_t box = { pos.x, (uint16_t)(text_y - font_h + 1) };
    if(compact)
    {
        point_t txt = { (uint16_t)(pos.x + 1), text_y };
        if(selected)
            gfx_drawRect(box, letter_w + 2, font_h + 1, color_white, true);
        gfx_print(txt, font, TEXT_ALIGN_LEFT,
                  selected ? color_black : color_white, "%s", letter);
        pos.x += letter_w + 2 + gap;
    }
    else
    {
        gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white, "%s", letter);
        pos.x += letter_w + gap;
        if(selected)
        {
            const char *tag = tx ? "TX" : "MAIN";
            uint16_t pad = (tag_w - gfx_getTextWidth(font, tag)) / 2;
            point_t txt = { (uint16_t)(pos.x + pad), text_y };
            box.x = pos.x;
            gfx_drawRect(box, tag_w, font_h + 1, text, true);
            gfx_print(txt, font, TEXT_ALIGN_LEFT, color_black, "%s", tag);
        }
        pos.x += tag_w + gap;
    }

    // Mode and BUSY flag
    point_t flag = { (uint16_t)(right - gfx_getTextWidth(font, "BUSY")),
                     text_y };
    if(compact)
    {
        point_t mode_pos = { (uint16_t)(right - gfx_getTextWidth(font, mode)),
                             text_y };
        point_t tx_pos = { (uint16_t)(right - gfx_getTextWidth(font, "TX")),
                           text_y };
        if(tx)
            gfx_print(tx_pos, font, TEXT_ALIGN_LEFT, text, "TX");
        else if(busy)
            gfx_print(flag, font, TEXT_ALIGN_LEFT, color_white, "BUSY");
        else
            gfx_print(mode_pos, font, TEXT_ALIGN_LEFT, color_white, "%s",
                      mode);
    }
    else
    {
        // Cut the status to the room left, before the BUSY flag if shown
        uint16_t room = (busy ? (uint16_t)(flag.x - gap) : right) - pos.x;
        size_t len = strlen(mode);
        while((len > 0) && (gfx_getTextWidth(font, mode) > room))
            mode[--len] = '\0';

        gfx_print(pos, font, TEXT_ALIGN_LEFT, color_white, "%s", mode);
        if(busy)
            gfx_print(flag, font, TEXT_ALIGN_LEFT, yellow_fab413, "BUSY");
    }

    // Frequency: full width under the status row, or between tag and mode
    freq_t freq = tx ? tuner_get_tx_frequency(t) : tuner_get_rx_frequency(t);
    if(compact && memory)
    {
        point_t name_pos = { pos.x, freq_y };
        gfx_print(name_pos, FONT_SIZE_8PT, TEXT_ALIGN_LEFT, text, "%.10s",
                  name);
    }
    else if(compact)
        _ui_printFrequency(freq, pos.x, flag.x - pos.x, freq_y,
                           FONT_SIZE_8PT, text);
    else
        _ui_printFrequency(freq, 0, CONFIG_SCREEN_WIDTH, freq_y,
                           layout.line3_large_font, text);
}

/**
 * Dual VFO: draw the two bands below the top bar, VFO A over VFO B.
 *
 * @param status: current RTX status.
 */
static void _ui_drawDualVfo(const rtxStatus_t *status)
{
    uint16_t top = layout.top_h + 1;
    uint16_t height = (CONFIG_SCREEN_HEIGHT - top) / 2;

    gfx_drawHLine(layout.top_h, layout.hline_h, color_grey);
    _ui_drawVfoBand(VFO_A, top, height, status);
    gfx_drawHLine(top + height, layout.hline_h, color_grey);
    _ui_drawVfoBand(VFO_B, top + height + 1, height - 1, status);
}

void _ui_drawVFOMiddleInput(ui_state_t* ui_state)
{
    // Add inserted number to string, skipping "Rx: "/"Tx: " and "."
    uint8_t insert_pos = ui_state->input_position + 3;
    if(ui_state->input_position > 3) insert_pos += 1;
    char input_char = ui_state->input_number + '0';

    if(ui_state->input_set == SET_RX)
    {
        if(ui_state->input_position == 0)
        {
            gfx_print(layout.line2_pos, layout.input_font, TEXT_ALIGN_CENTER,
                      color_white, ">Rx:%03lu.%04lu",
                      (unsigned long)ui_state->new_rx_frequency/1000000,
                      (unsigned long)(ui_state->new_rx_frequency%1000000)/100);
        }
        else
        {
            // Replace Rx frequency with underscorses
            if(ui_state->input_position == 1)
                strcpy(ui_state->new_rx_freq_buf, ">Rx:___.____");
            ui_state->new_rx_freq_buf[insert_pos] = input_char;
            gfx_print(layout.line2_pos, layout.input_font, TEXT_ALIGN_CENTER,
                      color_white, ui_state->new_rx_freq_buf);
        }
        gfx_print(layout.line3_large_pos, layout.input_font, TEXT_ALIGN_CENTER,
                  color_white, " Tx:%03lu.%04lu",
                  (unsigned long)tuner_get_tx_frequency(&last_state.tuner)/1000000,
                  (unsigned long)(tuner_get_tx_frequency(&last_state.tuner)%1000000)/100);
    }
    else if(ui_state->input_set == SET_TX)
    {
        gfx_print(layout.line2_pos, layout.input_font, TEXT_ALIGN_CENTER,
                  color_white, " Rx:%03lu.%04lu",
                  (unsigned long)ui_state->new_rx_frequency/1000000,
                  (unsigned long)(ui_state->new_rx_frequency%1000000)/100);
        // Replace Rx frequency with underscorses
        if(ui_state->input_position == 0)
        {
            gfx_print(layout.line3_large_pos, layout.input_font, TEXT_ALIGN_CENTER,
                      color_white, ">Tx:%03lu.%04lu",
                      (unsigned long)ui_state->new_rx_frequency/1000000,
                      (unsigned long)(ui_state->new_rx_frequency%1000000)/100);
        }
        else
        {
            if(ui_state->input_position == 1)
                strcpy(ui_state->new_tx_freq_buf, ">Tx:___.____");
            ui_state->new_tx_freq_buf[insert_pos] = input_char;
            gfx_print(layout.line3_large_pos, layout.input_font, TEXT_ALIGN_CENTER,
                      color_white, ui_state->new_tx_freq_buf);
        }
    }
}

void _ui_drawMainBottom()
{
    // Squelch bar
    rssi_t   rssi = last_state.rssi;
    uint8_t  squelch = tuner_get_sql_level(&last_state.tuner);
    uint8_t  volume = last_state.volume;
    uint16_t meter_width = CONFIG_SCREEN_WIDTH - 2 * layout.horizontal_pad;
    uint16_t meter_height = layout.bottom_h;
    point_t meter_pos = { layout.horizontal_pad,
                          CONFIG_SCREEN_HEIGHT - meter_height - layout.bottom_pad};
    uint8_t mic_level = platform_getMicLevel();
    switch(tuner_get_opmode(&last_state.tuner))
    {
        case OPMODE_FM:
            gfx_drawSmeter(meter_pos,
                           meter_width,
                           meter_height,
                           rssi,
                           squelch,
                           volume,
                           true,
                           yellow_fab413);
            break;
        case OPMODE_DMR:
            gfx_drawSmeterLevel(meter_pos,
                                meter_width,
                                meter_height,
                                rssi,
                                mic_level,
                                volume,
                                true);
            break;
        #ifdef CONFIG_M17
        case OPMODE_M17:
            gfx_drawSmeterLevel(meter_pos,
                                meter_width,
                                meter_height,
                                rssi,
                                mic_level,
                                volume,
                                true);
            break;
        #endif
    }
}

void _ui_drawMainVFO(ui_state_t* ui_state)
{
    gfx_clearScreen();
    _ui_drawMainTop(ui_state);

    // Show VFO frequency if the OpMode is not M17 or there is no valid LSF data
    rtxStatus_t status = rtx_getCurrentStatus();
    #ifdef CONFIG_M17
    bool showFreq = (status.opMode != OPMODE_M17) || (status.lsfOk == false);
    #else
    bool showFreq = true;
    #endif

    // Dual VFO: both bands take the place of the single VFO layout, which
    // stays in use while an M17 destination is being typed
    if(last_state.settings.dualVfo && showFreq && !ui_state->edit_mode)
    {
        _ui_drawDualVfo(&status);
        return;
    }

    _ui_drawModeInfo(ui_state);
    if(showFreq)
        _ui_drawFrequency();

    _ui_drawMainBottom();
}

void _ui_drawMainVFOInput(ui_state_t* ui_state)
{
    gfx_clearScreen();
    _ui_drawMainTop(ui_state);
    _ui_drawVFOMiddleInput(ui_state);
    _ui_drawMainBottom();
}

void _ui_drawMainMEM(ui_state_t* ui_state)
{
    gfx_clearScreen();
    _ui_drawMainTop(ui_state);

    // Show channel data if the OpMode is not M17 or there is no valid LSF data
    rtxStatus_t status = rtx_getCurrentStatus();
    #ifdef CONFIG_M17
    bool showFreq = (status.opMode != OPMODE_M17) || (status.lsfOk == false);
    #else
    bool showFreq = true;
    #endif

    // Dual VFO: the same two bands as in VFO mode, each tuner showing its
    // own frequency or channel
    if(last_state.settings.dualVfo && showFreq && !ui_state->edit_mode)
    {
        _ui_drawDualVfo(&status);
        return;
    }

    _ui_drawModeInfo(ui_state);
    if(showFreq)
    {
        _ui_drawBankChannel();
        _ui_drawFrequency();
    }

    _ui_drawMainBottom();
}
