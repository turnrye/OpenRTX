/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARROW_INPUT_CONTROL_H
#define ARROW_INPUT_CONTROL_H

#include "ui/InputControl.h"
#include <cstdint>

/** M17 callsign symbols — flat string of selectable characters. */
extern const char m17CallsignSymbols[];

/** Full text symbols — flat string for arrow-key selection. */
extern const char t9TextSymbols[];

/**
 * Arrow-key character selection input control (Module17 variant).
 *
 * UP/DOWN cycle through the symbol table at the current position.
 * LEFT deletes the current character and moves back.
 * RIGHT advances the cursor.
 *
 * The control operates on an external buffer passed via start().
 * It owns the cursor state but not the buffer.
 */
class ArrowInputControl : public InputControl
{
public:
    /**
     * Begin editing the given buffer.  Resets cursor state and clears
     * the buffer.  No underscore cursor is placed — the first character
     * is selected via UP/DOWN.
     *
     * @param buf     Buffer to edit (must be at least maxLen+1 bytes).
     * @param maxLen  Maximum number of input characters (excluding null).
     * @param symbols Flat string of selectable characters.
     * @param font    Font size to use when drawing the input text.
     */
    void start(char* buf, uint8_t maxLen, const char* symbols,
               fontSize_t font);

    void reset() override;
    InputResult handleKey(UIContext& ctx, event_t event) override;
    void draw(UIContext& ctx, bool overlay = true) override;

private:
    char* buf_              = nullptr;
    uint8_t maxLen_         = 0;
    const char* symbols_    = nullptr;
    fontSize_t font_        = FONT_SIZE_8PT;
    uint8_t input_position_ = 0;
    uint8_t input_set_      = 0;

    void confirm();
};

#endif // ARROW_INPUT_CONTROL_H
