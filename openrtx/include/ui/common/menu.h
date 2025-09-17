/***************************************************************************
 *   Copyright (C)        2025 by Federico Amedeo Izzo IU2NUO,             *
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

#ifndef UI_COMMON_MENU_H
#define UI_COMMON_MENU_H

#include <ui/ui_default.h>


/**
 * Function types for menu item value operations
 */
typedef int (*menu_item_getter_t)(char *buf, uint8_t max_len, void *user_data);
typedef int (*menu_item_setter_t)(const char *value, void *user_data);

/**
 * Struct representing a single menu item
 */
typedef struct menu_item_t {
    size_t lang_offset;                // Name for the menu item as an offset of field in stringsTable_t
    menu_item_getter_t get_value;      // Function to get item value
    menu_item_setter_t set_value;      // Function to set item value (NULL for read-only items)
    void *user_data;                   // Optional user data for the getter/setter
} menu_item_t;

/**
 * Struct representing a menu with its items and getter functions
 */
typedef struct menu_t {
    const char *title;              // Menu title (set at runtime from language)
    menu_item_t *items;             // Array of menu items
    uint8_t item_count;             // Number of items in the menu
    // TODO: add set_item_value
} menu_t;

// TODO: refactor to get rid of this
typedef struct {
    enum uiScreen screen;
    menu_t *menu;
} menu_mapping_t;

extern menu_mapping_t menu_mappings[];

int _ui_getMenuItemName(menu_t* menu, char *buf, uint8_t max_len, uint8_t index);
int _ui_getMenuItemNameWrapper(char *buf, uint8_t max_len, uint8_t index);
void _ui_drawMenuWithValues(ui_state_t* ui_state, menu_t* menu);

extern menu_t info_menu;

/**
 * Adapter function for menu_item_t to work with the existing API
 */
int _ui_getMenuItemValueAdapter(menu_t* menu, char *buf, uint8_t max_len, uint8_t index);

#endif /* UI_COMMON_MENU_H */