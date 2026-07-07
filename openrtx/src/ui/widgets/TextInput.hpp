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
 * Two deterministic (golden-testable) input methods drive the same buffer, and
 * the host picks per keypad capability: cursor-cycle (UP/DOWN cycle the char
 * through the Charset, LEFT/RIGHT or knob move) for arrow/knob radios, and ETSI
 * phone-style multi-tap (tapKey(): a digit types a letter, repeats cycle, a
 * different key or a lapsed window commits and advances) for numeric keypads.
 * The same state drives two renderers:
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

    /** Bind the ETSI multi-tap table for numeric-keypad entry (optional; only
     *  needed when the host routes digit keys to tapKey()). */
    void setMultiTap(const MultiTapTable &t)
    {
        tapTable_ = &t;
    }

    /* ---- Edit ops (driven by the host's key mapping) ---- */
    void cycle(int dir);      //< cycle the char under the cursor
    void moveCursor(int dir); //< move the cursor; right past the end extends
    void insert(uint32_t cp); //< insert a code point (UTF-8 picker seam)
    void backspace();         //< delete the char before the cursor
    void clear();             //< empty the buffer
    void stripTrailingSpaces();

    /** ETSI multi-tap: press of keypad key `keyIndex` (0-9, *=10, #=11) at tick
     *  `nowTick`. Same key within the window cycles the character in place; a
     *  different key or a lapsed window commits and advances (overwrite model).*/
    void tapKey(uint8_t keyIndex, long long nowTick);
    /** Finalize any in-progress multi-tap character (call before a cursor move
     *  or on commit) so the next tap starts a fresh character. */
    void commitPending();

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

    /* Multi-tap (ETSI keypad) state. */
    const MultiTapTable *tapTable_ = nullptr;
    bool tapActive_ = false; //< a character is mid-tap under the cursor
    uint8_t tapSet_ = 0;     //< index into the current key's cycle string
    int8_t tapKeyIdx_ = -1;  //< last keypad key tapped
    long long tapTick_ = 0;  //< tick of the last tap (for the window)
};

} // namespace ortxui

#endif /* ORTX_UI_TEXTINPUT_HPP */
