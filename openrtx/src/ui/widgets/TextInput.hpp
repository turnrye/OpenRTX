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
 * Deletion works on every radio: the cursor wheel has a trailing ⌫ slot, and
 * cycling a character to it then moving/committing removes it (keypad radios
 * also get '*' as a backspace shortcut). `putCodePoint()` is the UTF-8 character
 * picker seam for accented/symbol code points not on the keyboard; it inserts or
 * overwrites per the active EditMode, just like a typed character would.
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
     * Character-entry model. Overtype (the default, and the only model for the
     * inline editors) keeps a cell under the cursor and replaces it in place;
     * the cursor is a block. Insert places the cursor as a caret *between* cells
     * and pushes text right to make room, so characters can be added mid-string;
     * the cursor is a thin bar (a block only while a freshly inserted cell is
     * being dialed). Only the modal editor exposes a toggle between the two.
     */
    enum class EditMode : uint8_t { Overtype, Insert };

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

    /* ---- Mode ---- */
    void setEditMode(EditMode m); //< Overtype (default) or Insert
    EditMode editMode() const
    {
        return editMode_;
    }
    void toggleEditMode()
    {
        setEditMode(editMode_ == EditMode::Overtype ? EditMode::Insert :
                                                      EditMode::Overtype);
    }

    /* ---- Edit ops (driven by the host's key mapping) ---- */
    void cycle(int dir);      //< dial the cell (Insert: insert+dial a new cell)
    void moveCursor(int dir); //< move the cursor by one cell
    void
    putCodePoint(uint32_t cp); //< picker seam: insert or overwrite per mode
    void insert(uint32_t cp);  //< insert a code point (UTF-8), advancing
    void backspace();          //< delete the cell before the cursor
    void clear();              //< empty the buffer
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

    /** Compose a cursor-visible window of the bracketed buffer that fits `maxW`
     *  pixels at `font`, with a leading/trailing ellipsis (…) where content is
     *  truncated. For inline SingleLine editors whose field is narrower than
     *  the full content; the window scrolls to keep the cursor visible. Falls
     *  back to the full bracketed string when it already fits. */
    void formatWindow(char *out, size_t sz, uint16_t maxW,
                      fontSize_t font) const;

    /* ---- Object ---- */
    void draw(DrawCtx &d) override;

private:
    void ensureCursorVisibleV(uint16_t cursorRow, uint16_t rows,
                              uint16_t visRows);
    /* Compose the bracketed buffer restricted to cells [lo, hi) into `out`,
     * with a leading ellipsis when text precedes `lo` and a trailing one when
     * text follows `hi`. */
    void composeWindow(char *out, size_t sz, uint16_t lo, uint16_t hi) const;
    void deleteAt(uint16_t pos); //< remove the whole cell at `pos`
    void resolvePendingDelete(); //< apply a cycled-to ⌫ delete at the cursor

    /* UTF-8 cell stepping: a "cell" is one code point (1-3 bytes), so the cursor
     * and edits move over whole characters, not raw bytes -- picker-inserted
     * accents (2-byte Latin-1) then navigate and delete atomically. */
    uint16_t cellLen(uint16_t pos) const;  //< bytes of the cell starting at pos
    uint16_t nextCell(uint16_t pos) const; //< start of the cell after pos
    uint16_t prevCell(uint16_t pos) const; //< start of the cell before pos
    void spliceCell(uint16_t pos, const char *enc,
                    int n);                //< replace cell w/ enc
    void
    insertBlankAt(uint16_t pos); //< open a ' ' cell at pos (Insert dialing)

    char *buf_ = nullptr;
    uint16_t cap_ = 0;
    uint16_t len_ = 0;
    uint16_t cursor_ = 0;
    const Charset *cs_ = nullptr;
    Mode mode_ = Mode::SingleLine;
    CursorStyle cursorStyle_ = CursorStyle::Bracket;
    EditMode editMode_ = EditMode::Overtype;
    bool insertPending_ = false; //< Insert: cell at cursor_ is being dialed
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
