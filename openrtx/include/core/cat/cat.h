/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CAT_H
#define CAT_H

#include "interfaces/chardev.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CAT (computer aided transceiver) control, a cut-out of the Yaesu FT-991A
 * command set: two letters, fixed-width parameters, ';' terminator. A set is
 * silent, a read answers in the format of the set, and any error answers
 * "?;". CR and LF are ignored, so commands can be typed in a terminal.
 *
 * Yaesu commands, enough for Hamlib's FT-991 backend:
 *
 *   ID    identification, OpenRTX's own number 0999
 *   AI    auto information, always off
 *   PS    power switch, always on
 *   FA    RX frequency, 9 digits in Hz; a set keeps the repeater shift
 *   FB    TX frequency, 9 digits in Hz
 *   MD    mode, 4 FM or B FM narrow; NA narrow; SH width, always default
 *   IF    information record of the current channel
 *   OS    repeater shift, 0 simplex, 1 plus, 2 minus; a shifted channel
 *         keeps its offset, a simplex one takes the EX/ZO offset (600 kHz
 *         until set)
 *   EX    menus 080-083: the repeater offset, in kHz, whatever the band
 *   CN    CTCSS tone number, 000-049, OpenRTX's own index
 *   CT    tone mode, 0 off, 1 encode and decode, 2 encode
 *   PC    TX power, whole watts
 *   SQ    squelch, 000-100
 *   SM    S-meter, 000-255 on the FT-991A's scale
 *   TX    PTT, 0 receive, 1 transmit for at most two minutes; refused where
 *         the radio's own PTT is (menus, RX-only channels)
 *   BS    band select, FT TX VFO, EX032 CAT time-out: accepted, no effect
 *
 * Yaesu commands only see FM: MD, NA, IF, CN and CT answer "?;" on an M17
 * channel. The OpenRTX extensions carry the rest:
 *
 *   ZM    mode, 00 FM, 01 FM narrow, 02 M17
 *   ZP    TX power, 6 digits in mW
 *   ZO    repeater offset, 9 digits in Hz
 *   ZC    callsign, the M17 source
 *   ZD    M17 destination, ALL for the broadcast
 *   ZN    M17 channel access number, 00-15
 *   ZR    battery charge, percent
 *   ZV    firmware version
 *   ZB1   restart into the bootloader, on radios with CONFIG_BOOTLOADER_ENTRY
 */

/*
 * CAT changes the radio state and needs no code in the UI: the UI thread
 * applies a change to the radio (threads_requestRtxSync()) and the UI redraws
 * from the state. CAT tunes the VFO: a frequency change puts the radio in VFO
 * mode (state.tuner_mode), and in VFO mode the VFO copy follows every change.
 * The default UI does not follow tuner_mode: on its memory screen a CAT change
 * lasts until the next channel change, and also becomes the VFO that leaving
 * memory mode returns to.
 *
 * A target enables CAT with CONFIG_CAT and declares cat_chardev, the byte
 * stream CAT runs on, in its hwconfig.h.
 */

/**
 * Start CAT on a byte stream device.
 *
 * @param dev: device carrying CAT.
 * @return 0 on success, a negative error code if the device failed to start.
 */
int cat_init(const struct chardev_ops *dev);

/**
 * Execute the commands received since the last call and send their answers.
 * Called periodically from the main thread.
 *
 * @param now: current time in milliseconds.
 */
void cat_task(const long long now);

#ifdef __cplusplus
}
#endif

#endif // CAT_H
