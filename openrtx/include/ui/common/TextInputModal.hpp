/***************************************************************************
 *   Copyright (C) 2021 - 2025 by Federico Amedeo Izzo IU2NUO,             *
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

#ifndef UI_COMMON_TEXTINPUTMODAL_HPP
#define UI_COMMON_TEXTINPUTMODAL_HPP

#include <string>
#include <functional>
#include "core/graphics.h"
#include "ui/ui_default.h"

extern const color_t color_black;
extern const color_t color_grey;
extern const color_t color_white;
extern const color_t yellow_fab413;
extern layout_t layout;

class TextInputModal {
public:
    TextInputModal(const std::string& label,
             std::function<std::string()> getter,
             std::function<void(int)> adjust = nullptr,
             std::function<void()> select = nullptr,
             size_t maxLength = 9,
             bool isActive = false)
        : label(label), getter(getter), adjust(adjust), select(select), maxLength(maxLength), isActive(isActive) {}

    ~TextInputModal() {};

    void draw();

    const std::string& getLabel() const { return label; }

    std::string getValue() const { return getter ? getter() : ""; }

    void adjustValue(int delta) { if (adjust) adjust(delta); }
    
    void onSelect() { if (select) select(); }
private:
    std::string label;
    std::function<std::string()> getter;
    std::function<void(int)> adjust;
    std::function<void()> select;
    size_t maxLength;
    bool isActive;
};


#endif /* UI_COMMON_TEXTINPUTMODAL_HPP */
