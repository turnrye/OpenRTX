/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "hwconfig.h"

#ifdef CONFIG_APRS

#include <stdio.h>
#include <string.h>
#include "ui/ui_default.h"
#include "ui/ui_strings.h"
#include "core/aprs_traffic.h"
#include "core/aprs_qsy.h"
#include "core/stations.h"
#include "core/aprs_msg.h"
#include "protocols/APRS/symbols.h"
#include "interfaces/delays.h"

/* Generic menu list renderer, implementation is in "ui_menu.c". */
extern void _ui_drawMenuList(uint8_t selected,
                             int (*getCurrentEntry)(char *buf, uint8_t max_len,
                                                    uint8_t index));

static int _ui_getAprsMenuEntryName(char *buf, uint8_t max_len, uint8_t index)
{
    if (index >= aprs_menu_num)
        return -1;
    sniprintf(buf, max_len, "%s", aprs_menu_items[index]);
    return 0;
}

void _ui_drawMenuAPRS(ui_state_t *ui_state)
{
    gfx_clearScreen();
    gfx_print(layout.top_pos, layout.top_font, TEXT_ALIGN_CENTER, color_white,
              "APRS");
    _ui_drawMenuList(ui_state->menu_selected, _ui_getAprsMenuEntryName);
}

/** Two-letter kind, the way the Yaesu and Kenwood radios label packets. */
static const char *_ui_trafficKind(uint8_t type)
{
    switch (type) {
        case APRS_TYPE_MESSAGE:
            return "MS";
        case APRS_TYPE_POSITION:
            return "PO";
        case APRS_TYPE_STATUS:
            return "ST";
        case APRS_TYPE_OBJECT:
            return "OB";
        case APRS_TYPE_TELEMETRY:
            return "TE";
        case APRS_TYPE_WEATHER:
            return "WX";
        case APRS_TYPE_THIRD_PARTY:
            return "3P";
        default:
            return "??";
    }
}

/** '=' a repeat, '>' ours, '*' for us, else nothing. */
static char _ui_trafficMarker(uint8_t flags)
{
    if (flags & APRS_TRAFFIC_EMERGENCY)
        return '!';
    if (flags & APRS_TRAFFIC_DUP)
        return '=';
    if (flags & APRS_TRAFFIC_TX)
        return '>';
    if (flags & APRS_TRAFFIC_FOR_ME)
        return '*';
    if (flags & APRS_TRAFFIC_GROUP)
        return 'b';
    return ' ';
}

/** "12s", "3m" or "2h" since @p when, in seconds of system tick. */
static void _ui_ageStr(uint32_t when, char *buf, size_t len)
{
    uint32_t now = (uint32_t)(getTick() / 1000);
    uint32_t age = (now > when) ? (now - when) : 0;

    if (age < 60)
        sniprintf(buf, len, "%us", (unsigned)age);
    else if (age < 3600)
        sniprintf(buf, len, "%um", (unsigned)(age / 60));
    else
        sniprintf(buf, len, "%uh", (unsigned)(age / 3600));
}

static void _ui_trafficAge(const struct aprsTrafficEntry *e, char *buf,
                           size_t len)
{
    _ui_ageStr(e->heard, buf, len);
}

static int _ui_getTrafficEntryName(char *buf, uint8_t max_len, uint8_t index)
{
    const struct aprsTrafficEntry *e = aprsTraffic_get(index);
    if (e == NULL)
        return -1;

    char age[6];
    _ui_trafficAge(e, age, sizeof(age));
    sniprintf(buf, max_len, "%c%s %-9.9s %3s", _ui_trafficMarker(e->flags),
              _ui_trafficKind(e->type), e->sender, age);
    return 0;
}

void _ui_drawAprsTraffic(ui_state_t *ui_state)
{
    gfx_clearScreen();
    gfx_print(layout.top_pos, layout.top_font, TEXT_ALIGN_CENTER, color_white,
              currentLanguage->traffic);

    if (aprsTraffic_count() == 0) {
        gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                  color_white, currentLanguage->noTraffic);
        return;
    }

    _ui_drawMenuList(ui_state->menu_selected, _ui_getTrafficEntryName);
}

void _ui_drawAprsTrafficDetail(ui_state_t *ui_state)
{
    const struct aprsTrafficEntry *e = aprsTraffic_get(ui_state->menu_selected);

    gfx_clearScreen();
    if (e == NULL) {
        gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                  color_white, currentLanguage->noTraffic);
        return;
    }

    /* Inverted header: sender on the left, kind and age on the right */
    point_t hdr_rect = { 0, 0 };
    gfx_drawRect(hdr_rect, CONFIG_SCREEN_WIDTH, layout.top_h, color_white,
                 true);
    char age[6];
    char right[12];
    _ui_trafficAge(e, age, sizeof(age));
    sniprintf(right, sizeof(right), "%s %s", _ui_trafficKind(e->type), age);
    point_t hdr_left = { layout.horizontal_pad, layout.top_pos.y };
    gfx_print(hdr_left, layout.top_font, TEXT_ALIGN_LEFT, color_black,
              e->sender);
    gfx_print(layout.top_pos, layout.top_font, TEXT_ALIGN_RIGHT, color_black,
              right);

    /* The TNC2 line, scrolled like a message body */
    int16_t clip_top = (int16_t)layout.top_h;
    int16_t clip_bot = (int16_t)(CONFIG_SCREEN_HEIGHT - 1);
    uint8_t font_h = gfx_getFontHeight(layout.menu_font);
    uint16_t text_h =
        gfx_measureText(layout.menu_font, e->line, layout.horizontal_pad,
                        CONFIG_SCREEN_WIDTH - layout.horizontal_pad, SIZE_MAX);
    int16_t visible_h = clip_bot - clip_top;
    ui_state->detail_scroll_max = (text_h > (uint16_t)visible_h) ?
                                      (int16_t)(text_h - visible_h) :
                                      0;
    if (ui_state->detail_scroll > ui_state->detail_scroll_max)
        ui_state->detail_scroll = ui_state->detail_scroll_max;
    if (ui_state->detail_scroll < 0)
        ui_state->detail_scroll = 0;

    point_t text_start = { (int16_t)layout.horizontal_pad,
                           (int16_t)(clip_top + font_h
                                     - ui_state->detail_scroll) };
    gfx_printBufferClipped(text_start, layout.menu_font, TEXT_ALIGN_LEFT,
                           color_white, e->line,
                           CONFIG_SCREEN_WIDTH - layout.horizontal_pad,
                           clip_top, clip_bot);
}

/* ---- Stations ---- */

/** Our own position: the fixed one if set, else the GPS fix. */
static bool _ui_ownPosition(int32_t *lat, int32_t *lon)
{
    if (last_state.settings.beacon_pos_fixed) {
        *lat = last_state.settings.beacon_fixedLat;
        *lon = last_state.settings.beacon_fixedLon;
        return true;
    }

    const gps_t *g = &last_state.gps_data;
    if (!last_state.settings.gps_enabled
        || (g->fix_quality == FIX_QUALITY_NO_FIX)
        || (g->fix_quality == FIX_QUALITY_ESTIMATED))
        return false;

    *lat = g->latitude;
    *lon = g->longitude;
    return true;
}

/** "850m", "12.3k" or "123k". */
static void _ui_distanceStr(uint32_t metres, char *buf, size_t len)
{
    if (metres < 1000)
        sniprintf(buf, len, "%um", (unsigned)metres);
    else if (metres < 100000)
        sniprintf(buf, len, "%u.%uk", (unsigned)(metres / 1000),
                  (unsigned)((metres % 1000) / 100));
    else
        sniprintf(buf, len, "%uk", (unsigned)(metres / 1000));
}

static int _ui_getStationEntryName(char *buf, uint8_t max_len, uint8_t index)
{
    const struct station *s = stations_get(index);
    if (s == NULL)
        return -1;

    /* Distance when both positions are known, else how long ago */
    char right[8];
    int32_t lat, lon;
    if ((s->valid & STATION_HAS_POSITION) && _ui_ownPosition(&lat, &lon))
        _ui_distanceStr(stations_distance(lat, lon, s->latitude, s->longitude,
                                          NULL),
                        right, sizeof(right));
    else
        _ui_ageStr(s->lastHeard, right, sizeof(right));

    /* 'o' marks an object or item placed by someone else */
    sniprintf(buf, max_len, "%c%-9.9s %5s",
              (s->kind == STATION_KIND_STATION) ? ' ' : 'o', s->call, right);
    return 0;
}

void _ui_drawAprsStations(ui_state_t *ui_state)
{
    gfx_clearScreen();
    gfx_print(layout.top_pos, layout.top_font, TEXT_ALIGN_CENTER, color_white,
              currentLanguage->stations);

    if (stations_count() == 0) {
        gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                  color_white, currentLanguage->noStations);
        return;
    }

    _ui_drawMenuList(ui_state->menu_selected, _ui_getStationEntryName);
}

/** "49.0583N" from micro-degrees. */
static void _ui_coordStr(int32_t micro, char pos, char neg, char *buf,
                         size_t len)
{
    uint32_t mag = (micro < 0) ? (uint32_t)(-micro) : (uint32_t)micro;
    sniprintf(buf, len, "%lu.%04lu%c", (unsigned long)(mag / 1000000u),
              (unsigned long)((mag % 1000000u) / 100u),
              (micro < 0) ? neg : pos);
}

/* MHz with the decimals it needs, at least @p minDecimals: "441.1125" */
static size_t _ui_mhzStr(char *buf, size_t len, uint32_t hz, size_t minDecimals)
{
    char frac[8];
    sniprintf(frac, sizeof(frac), "%06lu", (unsigned long)(hz % 1000000));
    size_t d = 6;
    while ((d > minDecimals) && (frac[d - 1] == '0'))
        d--;
    frac[d] = '\0';

    int n = sniprintf(buf, len, (d > 0) ? "%lu.%s" : "%lu",
                      (unsigned long)(hz / 1000000), frac);
    return (n < 0) ? 0 : (size_t)n;
}

/* "146.625 -0.6 T107.2", "441.1125 M17 CAN0": the offset in MHz, as radios
 * show it */
static void _ui_freqStr(const struct aprsFreq *f, char *buf, size_t len)
{
    static const char *const MODES[] = { "FM",     "M17", "DMR",  "D-STAR",
                                         "Fusion", "P25", "NXDN", "Data" };

    size_t n = _ui_mhzStr(buf, len, f->freq, 3);
    if (n >= len)
        return;

    if (f->mode == APRS_FREQ_M17) {
        sniprintf(&buf[n], len - n, " M17 CAN%u", (unsigned)f->can);
        return;
    }
    if (f->mode != APRS_FREQ_FM) {
        sniprintf(&buf[n], len - n, " %s", MODES[f->mode % 8]);
        return;
    }

    int32_t off = aprsQsy_offset(f);
    if ((off != 0) && (n + 2 < len)) {
        buf[n++] = ' ';
        buf[n++] = (off < 0) ? '-' : '+';
        n += _ui_mhzStr(&buf[n], len - n, (uint32_t)((off < 0) ? -off : off),
                        0);
    }
    if (n >= len)
        return;

    uint8_t i = aprsQsy_toneIndex(f->tone);
    switch (f->toneType) {
        case APRS_TONE_ENCODE:
        case APRS_TONE_SQUELCH: {
            char c = (f->toneType == APRS_TONE_ENCODE) ? 'T' : 'C';
            if (i < CTCSS_FREQ_NUM)
                sniprintf(&buf[n], len - n, " %c%u.%u", c, ctcss_tone[i] / 10,
                          ctcss_tone[i] % 10);
            else
                sniprintf(&buf[n], len - n, " %c%u", c, f->tone);
            break;
        }
        case APRS_TONE_DCS:
            sniprintf(&buf[n], len - n, " D%03u", f->tone);
            break;
        case APRS_TONE_BURST:
            sniprintf(&buf[n], len - n, " 1750");
            break;
        default:
            break;
    }
}

/* Whether ENTER tunes to it, or why not */
static const char *_ui_tuneStr(const struct aprsFreq *f)
{
    switch (aprsQsy_tune(f, NULL)) {
        case APRS_QSY_OK:
            return "ENTER to tune";
        case APRS_QSY_MODE:
            return "Can't tune: mode";
        case APRS_QSY_TONE:
            if (f->toneType == APRS_TONE_DCS)
                return "Can't tune: DCS";
            if (f->toneType == APRS_TONE_BURST)
                return "Can't tune: 1750";
            return "Can't tune: tone";
        default:
            return "Can't tune: band";
    }
}

void _ui_drawAprsStationDetail(ui_state_t *ui_state)
{
    const struct station *s = stations_find(ui_state->station_call);

    gfx_clearScreen();
    if (s == NULL) {
        gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                  color_white, currentLanguage->noStations);
        return;
    }

    /* Inverted header: callsign left, age right */
    point_t hdr_rect = { 0, 0 };
    gfx_drawRect(hdr_rect, CONFIG_SCREEN_WIDTH, layout.top_h, color_white,
                 true);
    char age[6];
    _ui_ageStr(s->lastHeard, age, sizeof(age));
    point_t hdr_left = { layout.horizontal_pad, layout.top_pos.y };
    gfx_print(hdr_left, layout.top_font, TEXT_ALIGN_LEFT, color_black, s->call);
    gfx_print(layout.top_pos, layout.top_font, TEXT_ALIGN_RIGHT, color_black,
              age);

    /* One fact per line, only the facts the station gave */
    char line[32];
    point_t pos = layout.line1_pos;
    uint8_t lines = 0;

    if (s->valid & STATION_EMERGENCY) {
        /* Seeing the station is acknowledging its alarm */
        char pending[STATION_CALL_LEN];
        if (aprs_msg_emergency(pending, sizeof(pending), NULL)
            && (strcmp(pending, s->call) == 0))
            aprs_msg_emergencyAck();
        gfx_print(pos, layout.menu_font, TEXT_ALIGN_LEFT, color_white,
                  "!! EMERGENCY !!");
        pos.y += layout.menu_h;
        lines++;
    }

    /* First after an emergency: it is what ENTER acts on */
    if (s->valid & STATION_HAS_FREQ) {
        _ui_freqStr(&s->freq, line, sizeof(line));
        gfx_print(pos, layout.menu_font, TEXT_ALIGN_LEFT, color_white, line);
        pos.y += layout.menu_h;
        gfx_print(pos, layout.menu_font, TEXT_ALIGN_LEFT, color_white,
                  _ui_tuneStr(&s->freq));
        pos.y += layout.menu_h;
        lines += 2;
    }

    if (s->valid & STATION_HAS_SYMBOL) {
        int i = aprsSymbolFind(s->symTable, s->symCode);
        if (i >= 0)
            sniprintf(line, sizeof(line), "%s", aprsSymbols[i].name);
        else
            sniprintf(line, sizeof(line), "Symbol %c%c", s->symTable,
                      s->symCode);
        if (s->kind == STATION_KIND_OBJECT)
            strncat(line, " (object)", sizeof(line) - strlen(line) - 1);
        else if (s->kind == STATION_KIND_ITEM)
            strncat(line, " (item)", sizeof(line) - strlen(line) - 1);
        gfx_print(pos, layout.menu_font, TEXT_ALIGN_LEFT, color_white, line);
        pos.y += layout.menu_h;
        lines++;
    }

    if (s->valid & STATION_HAS_POSITION) {
        char lat[16], lon[16];
        _ui_coordStr(s->latitude, 'N', 'S', lat, sizeof(lat));
        _ui_coordStr(s->longitude, 'E', 'W', lon, sizeof(lon));
        sniprintf(line, sizeof(line), "%s %s%s", lat, lon,
                  (s->valid & STATION_POSITION_AMBIGUOUS) ? " ~" : "");
        gfx_print(pos, layout.menu_font, TEXT_ALIGN_LEFT, color_white, line);
        pos.y += layout.menu_h;
        lines++;
    }

    int32_t lat, lon;
    if ((s->valid & STATION_HAS_POSITION) && _ui_ownPosition(&lat, &lon)) {
        uint16_t bearing;
        uint32_t d = stations_distance(lat, lon, s->latitude, s->longitude,
                                       &bearing);
        char dist[8];
        _ui_distanceStr(d, dist, sizeof(dist));
        sniprintf(line, sizeof(line), "%s away, %u deg", dist,
                  (unsigned)bearing);
        gfx_print(pos, layout.menu_font, TEXT_ALIGN_LEFT, color_white, line);
        pos.y += layout.menu_h;
        lines++;
    }

    if (s->valid & STATION_HAS_VELOCITY) {
        sniprintf(line, sizeof(line), "%u km/h  %u deg", (unsigned)s->speed,
                  (unsigned)s->course);
        gfx_print(pos, layout.menu_font, TEXT_ALIGN_LEFT, color_white, line);
        pos.y += layout.menu_h;
        lines++;
    }

    if (s->valid & STATION_HAS_ALTITUDE) {
        sniprintf(line, sizeof(line), "%d m", (int)s->altitude);
        gfx_print(pos, layout.menu_font, TEXT_ALIGN_LEFT, color_white, line);
        pos.y += layout.menu_h;
        lines++;
    }

    if (lines == 0)
        gfx_print(pos, layout.menu_font, TEXT_ALIGN_LEFT, color_white,
                  "Heard, no position");
}

#endif /* CONFIG_APRS */
