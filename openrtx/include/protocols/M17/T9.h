/*
 * SPDX-FileCopyrightText: 2024 Wojciech Kaczmarski SP5WWP
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T9 predictive text entry
 * M17 Project, 6 November 2024
 */

#ifndef OPENRTX_INCLUDE_PROTOCOLS_M17_T9_H_
#define OPENRTX_INCLUDE_PROTOCOLS_M17_T9_H_

#include <stdint.h>
#include <string.h>

uint8_t getDigit(const char c);
char *getWord(char *dict, char *code);

#endif /* OPENRTX_INCLUDE_PROTOCOLS_M17_T9_H_ */
