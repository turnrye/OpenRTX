/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ORTX_UI_TEXTINPUT_HPP
#define ORTX_UI_TEXTINPUT_HPP

#include <cstdint>
#include <cstddef>
#include "core/Object.hpp"
#include "core/graphics.h"
#include "style/Charsets.hpp"

namespace ortxui
{

/**
 * Reusable text-input field over a caller-owned buffer.
 *
 * Editing is deterministic cursor-cycle (the platform gives only a key bitmask,
 * no T9), which keeps it golden-testable: UP/DOWN cycle the character under the
 * cursor through the configured Charset, LEFT/RIGHT move it (extending with a
 * space at the end), and the host maps keys to these ops. The same state drives
 * two renderers:
 *  - `formatBracketed()` composes an inline "W1[A]W" string for editors that
 *    live inside another widget (a list row or the VFO channel line);
 *  - `draw()` paints the field into its own area for a modal, with a block
 *    cursor and, in MultiLine mode, word-wrap + vertical scroll for long text
 *    (M17 SMS is ~821 chars). `DrawCtx` does not enforce its clip, so drawing
 *    self-limits to whole lines within `area_`.
 *
 * `insert()` is the injection point for a future UTF-8 character picker pushed
 * on top of the modal; today all built-in charsets are ASCII.
 */
class TextInput : public Object
{
public:
    enum class Mode : uint8_t { SingleLine, MultiLine };
    enum class CursorStyle : uint8_t {
        Bracket, //< inline "[X]" (formatBracketed); parity with the old editors
        Block,   //< inverse cell drawn by draw() (modal)
    };

    /**
     * Bind the field to `buf` (holds up to `cap`-1 chars + NUL) with charset
     * `cs`. `font` is used only by draw(). Call begin() next to seed the cursor.
     */
    void configure(char *buf, uint16_t cap, const Charset &cs, Mode mode,
                   CursorStyle cursor, fontSize_t font);

    /** Seed length/cursor from the current buffer; an empty buffer starts as a
     *  single editable blank so there is always a character under the cursor. */
    void begin();

    /* ---- Edit ops (driven by the host's key mapping) ---- */
    void cycle(int dir);      //< cycle the char under the cursor
    void moveCursor(int dir); //< move the cursor; right past the end extends
    void insert(uint32_t cp); //< insert a code point (UTF-8 picker seam)
    void backspace();         //< delete the char before the cursor
    void clear();             //< empty the buffer
    void stripTrailingSpaces();

    /* ---- Accessors ---- */
    uint16_t length() const
    {
        return len_;
    }
    uint16_t cursor() const
    {
        return cursor_;
    }
    char cursorChar() const
    {
        return (buf_ != nullptr && cursor_ < len_) ? buf_[cursor_] : ' ';
    }
    const char *text() const
    {
        return (buf_ != nullptr) ? buf_ : "";
    }

    /** Compose the buffer with the cursor character wrapped in brackets into
     *  `out` (SingleLine inline rendering). */
    void formatBracketed(char *out, size_t sz) const;

    /* ---- Object ---- */
    void draw(DrawCtx &d) override;

private:
    void ensureCursorVisibleV(uint16_t cursorRow, uint16_t rows,
                              uint16_t visRows);

    char *buf_ = nullptr;
    uint16_t cap_ = 0;
    uint16_t len_ = 0;
    uint16_t cursor_ = 0;
    const Charset *cs_ = nullptr;
    Mode mode_ = Mode::SingleLine;
    CursorStyle cursorStyle_ = CursorStyle::Bracket;
    fontSize_t font_ = FONT_SIZE_8PT;
    uint16_t topRow_ = 0; //< first visible wrapped row (MultiLine scroll)
};

} // namespace ortxui

#endif /* ORTX_UI_TEXTINPUT_HPP */
