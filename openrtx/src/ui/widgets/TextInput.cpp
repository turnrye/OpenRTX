/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "widgets/TextInput.hpp"
#include "render/DrawCtx.hpp"

#include <cstdio>
#include <cstring>

namespace ortxui
{

namespace
{
/* Longest wrapped line we measure/copy at once (a screen row never approaches
 * this many glyphs). */
constexpr uint16_t kLineBuf = 128;

/* Minimal UTF-8 encoder (BMP) for insert(); built-in charsets are ASCII so the
 * one-byte path is what runs today. Returns the byte count. */
int utf8Encode(uint32_t cp, char out[4])
{
    if (cp < 0x80u) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = static_cast<char>(0xC0u | (cp >> 6));
        out[1] = static_cast<char>(0x80u | (cp & 0x3Fu));
        return 2;
    }
    out[0] = static_cast<char>(0xE0u | (cp >> 12));
    out[1] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
    out[2] = static_cast<char>(0x80u | (cp & 0x3Fu));
    return 3;
}

/* Pixel width of buf[start, end) at the given font. */
uint16_t spanWidth(const char *buf, uint16_t start, uint16_t end,
                   fontSize_t font)
{
    char tmp[kLineBuf];
    uint16_t n = static_cast<uint16_t>(end - start);
    if (n >= sizeof(tmp))
        n = sizeof(tmp) - 1;
    memcpy(tmp, buf + start, n);
    tmp[n] = '\0';
    return gfx_getTextWidth(font, tmp);
}
} // namespace

void TextInput::configure(char *buf, uint16_t cap, const Charset &cs, Mode mode,
                          CursorStyle cursor, fontSize_t font)
{
    buf_ = buf;
    cap_ = cap;
    cs_ = &cs;
    mode_ = mode;
    cursorStyle_ = cursor;
    font_ = font;
    len_ = 0;
    cursor_ = 0;
    topRow_ = 0;
}

void TextInput::begin()
{
    if ((buf_ == nullptr) || (cap_ == 0))
        return;
    len_ = static_cast<uint16_t>(strnlen(buf_, cap_ - 1));
    buf_[len_] = '\0';
    if (len_ == 0) { /* always keep one editable blank under the cursor */
        buf_[0] = ' ';
        buf_[1] = '\0';
        len_ = 1;
    }
    cursor_ = 0;
    topRow_ = 0;
}

void TextInput::cycle(int dir)
{
    if ((buf_ == nullptr) || (cs_ == nullptr) || (cursor_ >= len_))
        return;
    const int idx = cs_->indexOf(buf_[cursor_]);
    buf_[cursor_] = cs_->at(idx + dir);
}

void TextInput::moveCursor(int dir)
{
    if (buf_ == nullptr)
        return;
    if (dir < 0) {
        if (cursor_ > 0)
            cursor_--;
    } else {
        if (cursor_ + 1 < len_) {
            cursor_++;
        } else if (len_ + 1 < cap_) {
            /* Extend with a blank and step onto it. */
            buf_[len_] = ' ';
            buf_[len_ + 1] = '\0';
            len_++;
            cursor_ = static_cast<uint16_t>(len_ - 1);
        }
    }
}

void TextInput::insert(uint32_t cp)
{
    if (buf_ == nullptr)
        return;
    char enc[4];
    const int n = utf8Encode(cp, enc);
    if (len_ + n + 1 > cap_) /* no room for the bytes plus the NUL */
        return;
    memmove(buf_ + cursor_ + n, buf_ + cursor_,
            static_cast<size_t>(len_ - cursor_) + 1); /* include NUL */
    for (int i = 0; i < n; i++)
        buf_[cursor_ + i] = enc[i];
    len_ = static_cast<uint16_t>(len_ + n);
    cursor_ = static_cast<uint16_t>(cursor_ + n);
}

void TextInput::backspace()
{
    if ((buf_ == nullptr) || (cursor_ == 0) || (len_ == 0))
        return;
    memmove(buf_ + cursor_ - 1, buf_ + cursor_,
            static_cast<size_t>(len_ - cursor_) + 1); /* include NUL */
    len_--;
    cursor_--;
}

void TextInput::clear()
{
    if (buf_ == nullptr)
        return;
    buf_[0] = '\0';
    len_ = 0;
    cursor_ = 0;
    topRow_ = 0;
}

void TextInput::stripTrailingSpaces()
{
    if (buf_ == nullptr)
        return;
    while ((len_ > 0) && (buf_[len_ - 1] == ' '))
        buf_[--len_] = '\0';
    if (cursor_ > len_)
        cursor_ = len_;
}

void TextInput::formatBracketed(char *out, size_t sz) const
{
    if ((out == nullptr) || (sz == 0))
        return;
    char *o = out;
    size_t rem = sz;
    if (buf_ != nullptr) {
        for (uint16_t i = 0; (i < len_) && (rem > 4); i++) {
            const int n = (i == cursor_) ? snprintf(o, rem, "[%c]", buf_[i]) :
                                           snprintf(o, rem, "%c", buf_[i]);
            if (n < 0)
                break;
            o += n;
            rem -= static_cast<size_t>(n);
        }
    }
    *o = '\0';
}

/* Exclusive end index of the wrapped line starting at `start` that fits `maxW`
 * pixels: break at the last space (word wrap), or mid-word when a single word
 * is too wide; always advances at least one character. */
static uint16_t lineEnd(const char *buf, uint16_t start, uint16_t len, int maxW,
                        fontSize_t font)
{
    if (start >= len)
        return start;
    uint16_t i = start;
    uint16_t lastSpace = start;
    bool haveSpace = false;
    while (i < len) {
        if (spanWidth(buf, start, static_cast<uint16_t>(i + 1), font) > maxW) {
            /* One glyph too wide: still advance by one to guarantee progress. */
            if (i == start)
                return static_cast<uint16_t>(start + 1);
            if (haveSpace)
                return static_cast<uint16_t>(lastSpace + 1);
            return i;
        }
        if (buf[i] == ' ') {
            lastSpace = i;
            haveSpace = true;
        }
        i++;
    }
    return len;
}

void TextInput::ensureCursorVisibleV(uint16_t cursorRow, uint16_t rows,
                                     uint16_t visRows)
{
    if (cursorRow < topRow_)
        topRow_ = cursorRow;
    else if (cursorRow >= topRow_ + visRows)
        topRow_ = static_cast<uint16_t>(cursorRow - visRows + 1);
    if (topRow_ + visRows > rows)
        topRow_ = (rows > visRows) ? static_cast<uint16_t>(rows - visRows) : 0;
}

void TextInput::draw(DrawCtx &d)
{
    if ((buf_ == nullptr) || area_.empty())
        return;

    const int16_t fh = static_cast<int16_t>(gfx_getFontHeight(font_));
    const int16_t lh = static_cast<int16_t>(fh + 2);

    auto drawCursor = [&](const Rect &lineBox, uint16_t lineStart) {
        const uint16_t preW = spanWidth(buf_, lineStart, cursor_, font_);
        const int16_t cx = static_cast<int16_t>(area_.x + preW);
        const bool onChar = (cursor_ < len_) && (buf_[cursor_] != ' ');
        char t[2] = { onChar ? buf_[cursor_] : ' ', '\0' };
        uint16_t cw = onChar ? gfx_getTextWidth(font_, t) : 3u;
        if (cw < 2u)
            cw = 2u;
        const Rect cell = { cx, lineBox.y, cw, lineBox.h };
        d.fillRect(cell, Sem::Primary);
        if (onChar)
            d.textInBox(cell, font_, TEXT_ALIGN_LEFT, Sem::OnPrimary, t);
    };

    if (mode_ == Mode::SingleLine) {
        /* Assumes the content fits the field width (callsign/dest-length use);
         * long text uses MultiLine. */
        d.textInBox(area_, font_, TEXT_ALIGN_LEFT, Sem::OnSurface, buf_);
        drawCursor(area_, 0);
        return;
    }

    const int maxW = static_cast<int>(area_.w) - 3;
    uint16_t visRows =
        static_cast<uint16_t>(area_.h / static_cast<uint16_t>(lh));
    if (visRows == 0)
        visRows = 1;

    /* Pass 1: count wrapped rows and locate the cursor's row. */
    uint16_t rows = 0, cursorRow = 0, pos = 0;
    while (true) {
        const uint16_t end = lineEnd(buf_, pos, len_, maxW, font_);
        if ((cursor_ >= pos) && (cursor_ < end))
            cursorRow = rows;
        else if ((end >= len_) && (cursor_ == len_))
            cursorRow = rows;
        rows++;
        if (end >= len_)
            break;
        pos = end;
    }
    ensureCursorVisibleV(cursorRow, rows, visRows);

    /* Pass 2: draw the visible rows and the cursor. */
    pos = 0;
    for (uint16_t r = 0;; r++) {
        const uint16_t end = lineEnd(buf_, pos, len_, maxW, font_);
        if ((r >= topRow_) && (r < topRow_ + visRows)) {
            const int16_t rowY =
                static_cast<int16_t>(area_.y + (r - topRow_) * lh);
            const Rect lineBox = { area_.x, rowY, area_.w,
                                   static_cast<uint16_t>(lh) };
            char tmp[kLineBuf];
            uint16_t n = static_cast<uint16_t>(end - pos);
            if (n >= sizeof(tmp))
                n = sizeof(tmp) - 1;
            memcpy(tmp, buf_ + pos, n);
            tmp[n] = '\0';
            d.textInBox(lineBox, font_, TEXT_ALIGN_LEFT, Sem::OnSurface, tmp);
            if (r == cursorRow)
                drawCursor(lineBox, pos);
        }
        if (end >= len_)
            break;
        pos = end;
    }

    /* Scroll indicator (mirrors List): a track with a thumb. */
    if (rows > visRows) {
        const int16_t barX = static_cast<int16_t>(area_.right() - 1);
        const int trackH = static_cast<int>(area_.h);
        int thumbH = trackH * visRows / rows;
        if (thumbH < 2)
            thumbH = 2;
        const int16_t thumbY =
            static_cast<int16_t>(area_.y + trackH * topRow_ / rows);
        const Rect track = { barX, area_.y, 2, area_.h };
        d.fillRect(track, Sem::Surface);
        const Rect thumb = { barX, thumbY, 2, static_cast<uint16_t>(thumbH) };
        d.fillRect(thumb, Sem::OnSurfaceMuted);
    }
}

} // namespace ortxui
