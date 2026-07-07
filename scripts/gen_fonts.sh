#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regenerate the committed LVGL binary UI fonts under openrtx/fonts/.
#
# The firmware build does NOT run this — it embeds the committed .bin blobs
# directly (openrtx/src/core/fontData.S). Run this only to change the font,
# the pixel sizes, or the icon set, then commit the regenerated .bin files.
#
# Fonts are produced by lv_font_conv (https://github.com/lvgl/lv_font_conv),
# invoked through `npx` so no global install is required.
#
# Usage:  scripts/gen_fonts.sh [UBUNTU_TTF] [FONTAWESOME_WOFF]
#
# Sources (defaults, override via the two positional args):
#   - Text:  Ubuntu-R.ttf (Ubuntu Font Licence 1.0), e.g. the fonts-ubuntu pkg.
#   - Icons: FontAwesome5-Solid+Brands+Regular.woff (SIL OFL 1.1), from
#     https://github.com/lvgl/lvgl/tree/master/scripts/built_in_font
set -euo pipefail

TTF="${1:-/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf}"
FA="${2:-FontAwesome5-Solid+Brands+Regular.woff}"
OUT="$(cd "$(dirname "$0")/../openrtx/fonts" && pwd)"

command -v npx >/dev/null || { echo "npx (Node.js) is required"; exit 1; }
[ -f "$TTF" ] || { echo "Ubuntu TTF not found: $TTF"; exit 1; }
[ -f "$FA" ]  || { echo "FontAwesome woff not found: $FA"; exit 1; }

# fontSize_t slot -> lv_font_conv pixel size (ppem). Calibrated so each font's
# digit advance and cap height match the historic Adafruit GFXfont metrics, so
# existing layouts do not reflow. NOTE: ppem != the nominal "pt" name.
declare -A PPEM=( [5]=8 [6]=12 [8]=16 [9]=17 [10]=20 [12]=25 [16]=30 )

# ASCII text plus a curated FontAwesome icon subset (LVGL's canonical
# codepoints, all present in the free woff). Extend as the UI needs icons.
ASCII="0x20-0x7E"
ICONS="0xF013,0xF023,0xF026,0xF028,0xF071,0xF095,0xF0C9,0xF0E0,0xF0E7,0xF0F3,0xF124,0xF1EB,0xF240,0xF242,0xF244"

for pt in 5 6 8 9 10 12 16; do
    p="${PPEM[$pt]}"
    # 1bpp (monochrome targets), raw.
    npx --yes lv_font_conv@latest \
        --font "$TTF" -r "$ASCII" --font "$FA" -r "$ICONS" \
        --size "$p" --bpp 1 --format bin --no-compress --no-kerning \
        -o "$OUT/ubuntu_${pt}_1.bin"
    # 4bpp (colour targets), RLE-compressed, anti-aliased.
    npx --yes lv_font_conv@latest \
        --font "$TTF" -r "$ASCII" --font "$FA" -r "$ICONS" \
        --size "$p" --bpp 4 --format bin --no-kerning \
        -o "$OUT/ubuntu_${pt}_4.bin"
    echo "ubuntu_${pt}  (ppem $p): $(stat -c%s "$OUT/ubuntu_${pt}_1.bin")B 1bpp, $(stat -c%s "$OUT/ubuntu_${pt}_4.bin")B 4bpp"
done
echo "Regenerated $OUT/ubuntu_*.bin — commit the changes."
