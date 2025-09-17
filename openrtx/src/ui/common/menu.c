/***************************************************************************
 *   Copyright (C) 2020 - 2025 by Federico Amedeo Izzo IU2NUO,             *
 *                                Niccolò Izzo IU2KIN                      *
 *                                Frederik Saraci IU2NRO                   *
 *                                Silvano Seva IU2KWO                      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, see <http://www.gnu.org/licenses/>   *
 ***************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <utils.h>
#include <ui/ui_default.h>
#include <interfaces/nvmem.h>
#include <interfaces/cps_io.h>
#include <interfaces/platform.h>
#include <memory_profiling.h>
#include <ui/ui_strings.h>
#include <core/voicePromptUtils.h>
#include <ui/common/menu.h>

/**
 * Common menu implementations for OpenRTX UI
 */

// Forward declarations for getter functions defined in ui_menu.c
void _ui_drawMenuListValue(ui_state_t* ui_state, uint8_t selected,
                           int (*getCurrentEntry)(char *buf, uint8_t max_len, uint8_t index),
                           int (*getCurrentValue)(char *buf, uint8_t max_len, uint8_t index));


// TODO: refactor the existing ui_state to just track a menu_t rather than int and get rid of this whole current_menu thing
static menu_t* current_menu = NULL;

// TODO: once global state tracks a menu_t this menu_mapping can be removed
menu_mapping_t menu_mappings[] = {
    { MENU_INFO, &info_menu }
};

int _ui_getMenuItemName(menu_t* menu, char *buf, uint8_t max_len, uint8_t index)
{
    if(index >= menu->item_count) return -1;
    
    // If we have a language offset, use it to get the string from currentLanguage
    if(menu->items[index].lang_offset > 0) {
        const char *str = *(const char **)((char *)currentLanguage + menu->items[index].lang_offset);
        sniprintf(buf, max_len, "%s", str);
    } 
    // Empty string if not available
    else {
        sniprintf(buf, max_len, "");
    }
    return 0;
}
/**
 * Adapter function to get menu item value using the new structure
 */
int _ui_getMenuItemValueAdapter(menu_t* menu, char *buf, uint8_t max_len, uint8_t index)
{
    if(index >= menu->item_count) return -1;
    
    if(menu->items[index].get_value) {
        return menu->items[index].get_value(buf, max_len, menu->items[index].user_data);
    }
    return -1;
}

// TODO: refactor to get rid of menu_mappings and the need for this
int _ui_getMenuItemNameWrapper(char *buf, uint8_t max_len, uint8_t index)
{
    // If current_menu is set, use that instead of looking up by screen
    if (current_menu != NULL) {
        return _ui_getMenuItemName(current_menu, buf, max_len, index);
    }
    
    // Otherwise fall back to menu mapping
    menu_t* lookup_menu = NULL;
    
    // Find the appropriate menu for the current screen
    for (size_t i = 0; i < sizeof(menu_mappings) / sizeof(menu_mapping_t); i++)
    {
        if (menu_mappings[i].screen == state.ui_screen)
        {
            lookup_menu = menu_mappings[i].menu;
            break;
        }
    }
    
    if (lookup_menu == NULL)
        return -1;
    
    return _ui_getMenuItemName(lookup_menu, buf, max_len, index);
}


static void _ui_setCurrentMenu(menu_t* menu)
{
    current_menu = menu;
}

static int _ui_getMenuItemValueWrapper(char *buf, uint8_t max_len, uint8_t index)
{
    if (current_menu == NULL) return -1;
    return _ui_getMenuItemValueAdapter(current_menu, buf, max_len, index);
}

void _ui_drawMenuWithValues(ui_state_t* ui_state, menu_t* menu)
{
    gfx_clearScreen();
    
    // Update the current menu pointer for wrapper functions
    _ui_setCurrentMenu(menu);
    
    // Print menu title on top bar
    gfx_print(layout.top_pos, layout.top_font, TEXT_ALIGN_CENTER,
              color_white, menu->title);
    
    // Print menu entries using the struct, with our wrapper for menu item values
    _ui_drawMenuListValue(ui_state, ui_state->menu_selected, 
                          _ui_getMenuItemNameWrapper,
                          _ui_getMenuItemValueWrapper);
}



/**
 * Code specifically related to the Info menu now
 */
static int _get_git_version(char *buf, uint8_t max_len, void *user_data)
{
    sniprintf(buf, max_len, "%s", GIT_VERSION);
    return 0;
}

static int _get_battery_voltage(char *buf, uint8_t max_len, void *user_data)
{
    // Compute integer part and mantissa of voltage value, adding 50mV
    // to mantissa for rounding to nearest integer
    uint16_t volt  = (last_state.v_bat + 50) / 1000;
    uint16_t mvolt = ((last_state.v_bat - volt * 1000) + 50) / 100;
    sniprintf(buf, max_len, "%d.%dV", volt, mvolt);
    return 0;
}

static int _get_battery_charge(char *buf, uint8_t max_len, void *user_data)
{
    sniprintf(buf, max_len, "%d%%", last_state.charge);
    return 0;
}

static int _get_rssi(char *buf, uint8_t max_len, void *user_data)
{
    sniprintf(buf, max_len, "%"PRIi32"dBm", last_state.rssi);
    return 0;
}

static int _get_heap_usage(char *buf, uint8_t max_len, void *user_data)
{
    sniprintf(buf, max_len, "%dB", getHeapSize() - getCurrentFreeHeap());
    return 0;
}

static int _get_band_info(char *buf, uint8_t max_len, void *user_data)
{
    const hwInfo_t* hwinfo = platform_getHwInfo();
    sniprintf(buf, max_len, "%s %s", 
             hwinfo->vhf_band ? currentLanguage->VHF : "", 
             hwinfo->uhf_band ? currentLanguage->UHF : "");
    return 0;
}

static int _get_vhf_info(char *buf, uint8_t max_len, void *user_data)
{
    const hwInfo_t* hwinfo = platform_getHwInfo();
    sniprintf(buf, max_len, "%d - %d", hwinfo->vhf_minFreq, hwinfo->vhf_maxFreq);
    return 0;
}

static int _get_uhf_info(char *buf, uint8_t max_len, void *user_data)
{
    const hwInfo_t* hwinfo = platform_getHwInfo();
    sniprintf(buf, max_len, "%d - %d", hwinfo->uhf_minFreq, hwinfo->uhf_maxFreq);
    return 0;
}

static int _get_hw_version(char *buf, uint8_t max_len, void *user_data)
{
    const hwInfo_t* hwinfo = platform_getHwInfo();
    sniprintf(buf, max_len, "%d", hwinfo->hw_version);
    return 0;
}

#ifdef PLATFORM_TTWRPLUS
static int _get_radio_model(char *buf, uint8_t max_len, void *user_data)
{
    strncpy(buf, sa8x8_getModel(), max_len);
    return 0;
}

static int _get_radio_firmware(char *buf, uint8_t max_len, void *user_data)
{
    // Get FW version string, skip the first nine chars ("sa8x8-fw/")
    uint8_t major, minor, patch, release;
    const char *fwVer = sa8x8_getFwVersion();

    sscanf(fwVer, "sa8x8-fw/v%hhu.%hhu.%hhu.r%hhu", &major, &minor, &patch, &release);
    sniprintf(buf, max_len,"v%hhu.%hhu.%hhu.r%hhu", major, minor, patch, release);
    return 0;
}
#endif

#ifdef PLATFORM_MOD17
static int _get_hmi_version(char *buf, uint8_t max_len, void *user_data)
{
    const hwInfo_t* hwinfo = platform_getHwInfo();
#ifdef PLATFORM_LINUX
    snprintf(buf, max_len, "%s", "Linux");
#else
    if(hwinfo->flags & MOD17_FLAGS_HMI_PRESENT)
        snprintf(buf, max_len, "%s", hmiVersions[(hwinfo->hw_version >> 8) & 0xFF]);
    else
        snprintf(buf, max_len, "%s", hmiVersions[0]);
#endif
    return 0;
}

static int _get_bb_tuning_pot(char *buf, uint8_t max_len, void *user_data)
{
    const hwInfo_t* hwinfo = platform_getHwInfo();
#ifdef PLATFORM_LINUX
    snprintf(buf, max_len, "%s", "Linux");
#else
    if((hwinfo->flags & MOD17_FLAGS_SOFTPOT) != 0)
        snprintf(buf, max_len, "%s", bbTuningPot[0]);
    else
        snprintf(buf, max_len, "%s", bbTuningPot[1]);
#endif
    return 0;
}
#endif

menu_item_t info_menu_items[] = {
    { 0, _get_git_version, NULL, NULL },
#ifndef PLATFORM_MOD17
    { offsetof(stringsTable_t, batteryVoltage), _get_battery_voltage, NULL, NULL },
    { offsetof(stringsTable_t, batteryCharge), _get_battery_charge, NULL, NULL },
    { offsetof(stringsTable_t, RSSI), _get_rssi, NULL, NULL },
#endif /* PLATFORM_MOD17 */
    { offsetof(stringsTable_t, usedHeap), _get_heap_usage, NULL, NULL },
#ifndef PLATFORM_MOD17
    { offsetof(stringsTable_t, band), _get_band_info, NULL, NULL },
    { offsetof(stringsTable_t, VHF), _get_vhf_info, NULL, NULL },
    { offsetof(stringsTable_t, UHF), _get_uhf_info, NULL, NULL },
#endif /* PLATFORM_MOD17 */
    // { offsetof(stringsTable_t, HWVersion), _get_hw_version, NULL, NULL }, // TODO: add string HWVersion
#ifdef PLATFORM_TTWRPLUS
    // { offsetof(stringsTable_t, radio), _get_radio_model, NULL, NULL }, // TODO: add string radio
    // { offsetof(stringsTable_t, radioFW), _get_radio_firmware, NULL, NULL }, // TODO: add string radioFW
#endif
#ifdef PLATFORM_MOD17
    // { offsetof(stringsTable_t, HMI), _get_hmi_version, NULL, NULL }, // TODO: add string HMI
    // { offsetof(stringsTable_t, bbTuningPot), _get_bb_tuning_pot, NULL, NULL }, // TODO: add string bbTuningPot
#endif /* PLATFORM_MOD17 */
};

const uint8_t info_num = sizeof(info_menu_items)/sizeof(info_menu_items[0]);

// Menu structure definitions
menu_t info_menu = {
    .title = NULL,  // Will be set at runtime from currentLanguage
    .items = info_menu_items,
    .item_count = 0,  // Will be set at runtime since this isn't constant
};

void _ui_drawMenuInfo(ui_state_t* ui_state)
{
    // Set title from current language before using
    info_menu.title = currentLanguage->info;
    info_menu.item_count = info_num;

    // Use the generic function
    _ui_drawMenuWithValues(ui_state, &info_menu);
}

// Maintain backward compatibility with other code
int _ui_getInfoValueName(char *buf, uint8_t max_len, uint8_t index)
{
    return _ui_getMenuItemValueAdapter(&info_menu, buf, max_len, index);
}