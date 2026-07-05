/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_PALETTE_HPP
#define ORTX_UI_PALETTE_HPP

#include "core/graphics.h"

namespace ortxui
{
namespace palette
{

/**
 * Build an opaque color_t from 8-bit R/G/B components.
 */
constexpr color_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return color_t{ r, g, b, 0xFF };
}

/*
 * The "Instrument" raw palette. These are the concrete colours the semantic
 * roles resolve to on colour (RGB565) targets. Iterate these values live in
 * the linux emulator (true RGB565); do a daylight hardware pass before
 * locking the muted greys and meter gradient.
 */
constexpr color_t background = rgb(0x0E, 0x0F, 0x12);   //< near-black charcoal
constexpr color_t surface = rgb(0x1A, 0x1C, 0x20);      //< elevation step
constexpr color_t surfaceHigh = rgb(0x26, 0x29, 0x2E);  //< selected row
constexpr color_t onSurface = rgb(0xF2, 0xF2, 0xF0);    //< warm off-white
constexpr color_t onSurfaceMut = rgb(0x8A, 0x8F, 0x98); //< muted grey
constexpr color_t brandGold = rgb(0xFA, 0xB4, 0x13);    //< OpenRTX accent
constexpr color_t txRed = rgb(0xE5, 0x34, 0x2B);        //< TX / danger
constexpr color_t rxGreen = rgb(0x37, 0xC8, 0x71);      //< RX / squelch-open
constexpr color_t warnOrange = rgb(0xF5, 0x73, 0x1E);   //< battery-low
constexpr color_t m17Red = rgb(0xFF, 0x00, 0x00);       //< M17 brand red
constexpr color_t m17Grey = rgb(0x99, 0x99, 0x99);      //< M17 logo accent
constexpr color_t fmCyanGrey = rgb(0x7F, 0xA6, 0xB8);   //< FM mode badge

/* Ink/paper extremes for 1bpp monochrome targets. */
constexpr color_t black = rgb(0x00, 0x00, 0x00);
constexpr color_t white = rgb(0xFF, 0xFF, 0xFF);

} // namespace palette
} // namespace ortxui

#endif /* ORTX_UI_PALETTE_HPP */
