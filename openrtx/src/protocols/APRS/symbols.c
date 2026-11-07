/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/APRS/symbols.h"

/* Codes from the APRS symbol table of github.com/wb2osz/aprsspec. */
const struct aprsSymbol aprsSymbols[] = {
    { '/', '[', "Person" },     { '/', '>', "Car" },
    { '/', '-', "House" },      { '/', ';', "Portable" },
    { '/', '<', "Motorcycle" }, { '/', 'b', "Bicycle" },
    { '/', 'j', "Jeep" },       { '/', 'k', "Truck" },
    { '/', 'v', "Van" },        { '/', 'R', "RV" },
    { '/', 'U', "Bus" },        { '/', 'u', "Semi truck" },
    { '/', 's', "Boat" },       { '/', 'Y', "Sailboat" },
    { '/', '\'', "Aircraft" },  { '/', 'X', "Helicopter" },
    { '/', 'O', "Balloon" },    { '/', 'a', "Ambulance" },
    { '/', 'f', "Fire truck" }, { '/', '_', "Weather" },
    { '/', 'r', "Repeater" },   { '/', '#', "Digi" },
};

const size_t aprsSymbolsNum = sizeof(aprsSymbols) / sizeof(aprsSymbols[0]);

int aprsSymbolFind(char table, char code)
{
    for (size_t i = 0; i < aprsSymbolsNum; i++) {
        if ((aprsSymbols[i].table == table) && (aprsSymbols[i].code == code))
            return (int)i;
    }

    return -1;
}
