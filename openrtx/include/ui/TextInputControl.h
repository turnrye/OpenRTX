/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TEXT_INPUT_CONTROL_H
#define TEXT_INPUT_CONTROL_H

#include "ui/InputControl.h"
#include <cstdint>

/**
 * Symbol table type, determined at compile time by input method.
 *
 * Default (T9 keypad): array of strings, one per key (0-9, *, #).
 * Module17 (arrow keys): flat string of selectable characters.
 */
#ifdef CONFIG_UI_MODULE17
using SymbolTable = const char*;
#else
using SymbolTable = const char* const*;
#endif

/** M17 callsign symbols (A-Z, 0-9, space, -, /, .). */
extern const SymbolTable m17CallsignSymbols;

/**
 * Reusable text input control.
 *
 * Supports two input methods selected at compile time:
 *   - T9 multi-tap keypad (default variant)
 *   - Arrow-key character selection (module17 variant)
 *
 * The control operates on an external buffer passed via start().
 * It owns the cursor state but not the buffer.
 *
 * The symbol table passed to start() determines which characters are
 * available.  Use m17CallsignSymbols for callsign entry.  A future
 * full-text table (e.g. ITU-T E.161 with punctuation and lowercase)
 * can be defined and passed to the same control.
 */
class TextInputControl : public InputControl
{
public:
    /**
     * Begin editing the given buffer.  Resets cursor state, clears the
     * buffer, and places an underscore cursor at position 0.
     *
     * @param buf     Buffer to edit (must be at least maxLen+1 bytes).
     * @param maxLen  Maximum number of input characters (excluding null).
     * @param symbols Symbol table defining available characters.
     */
    void start(char* buf, uint8_t maxLen, SymbolTable symbols);

    /**
     * Clear cursor state only (position, character set, keypress timer).
     * Does not touch the buffer — prefer start() to begin a new editing
     * session.  Required by the InputControl interface.
     */
    void reset() override;

    /**
     * Process a single key event while the control is active.
     *
     * ENTER confirms input, ESC cancels.  Other keys are interpreted
     * according to the active input method (T9 or arrow).
     *
     * @param ctx    Shared UI context (currently unused, reserved).
     * @param event  The keyboard event to handle.
     * @return Confirmed when the user pressed ENTER,
     *         Cancelled on ESC, Ongoing otherwise.
     */
    InputResult handleKey(UIContext& ctx, event_t event) override;

    /**
     * Draw the current buffer text, optionally inside an overlay rectangle.
     *
     * @param ctx     Shared UI context (provides layout metrics).
     * @param overlay When true a bordered rectangle is drawn behind the text.
     */
    void draw(UIContext& ctx, bool overlay = true) override;

private:
    char* buf_              = nullptr;
    uint8_t maxLen_         = 0;
    SymbolTable symbols_    = nullptr;
    uint8_t input_position_ = 0;
    uint8_t input_set_      = 0;
    uint8_t input_number_   = 0;
    long long last_keypress_ = 0;

    /** Null-terminate the buffer after the last entered character. */
    void confirm();

    /** Delete the character at the current position and move the cursor back. */
    void handleDelete();
};

#endif // TEXT_INPUT_CONTROL_H
