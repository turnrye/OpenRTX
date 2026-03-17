/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef T9_INPUT_CONTROL_H
#define T9_INPUT_CONTROL_H

#include "ui/InputControl.h"
#include <cstdint>

/** M17 callsign symbols — one string per T9 key (0–9, *, #). */
extern const char* const m17CallsignSymbols[];

/** Full text symbols — ITU-T E.161 T9 keypad layout. */
extern const char* const t9TextSymbols[];

/**
 * T9 multi-tap keypad text input control.
 *
 * Each numeric key maps to a string of characters.  Pressing the same
 * key repeatedly cycles through its characters; pressing a different
 * key advances the cursor.  Arrow keys act as backspace.
 *
 * The control operates on an external buffer passed via start().
 * It owns the cursor state but not the buffer.
 */
class T9InputControl : public InputControl
{
public:
    /**
     * Begin editing the given buffer.  Resets cursor state, clears the
     * buffer, and places an underscore cursor at position 0.
     *
     * @param buf     Buffer to edit (must be at least maxLen+1 bytes).
     * @param maxLen  Maximum number of input characters (excluding null).
     * @param symbols Symbol table — array of strings, one per key.
     * @param font    Font size to use when drawing the input text.
     */
    void start(char* buf, uint8_t maxLen, const char* const* symbols,
               fontSize_t font);

    void reset() override;
    InputResult handleKey(UIContext& ctx, event_t event) override;
    void draw(UIContext& ctx, bool overlay = true) override;

private:
    char* buf_               = nullptr;
    uint8_t maxLen_          = 0;
    const char* const* symbols_ = nullptr;
    fontSize_t font_         = FONT_SIZE_8PT;
    uint8_t input_position_  = 0;
    uint8_t input_set_       = 0;
    uint8_t input_number_    = 0;
    long long last_keypress_ = 0;

    void confirm();
    void handleDelete();
};

#endif // T9_INPUT_CONTROL_H
