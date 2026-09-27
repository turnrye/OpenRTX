/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PARAMP_CS7000_H
#define PARAMP_CS7000_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * PA ramp of the DMR bursts on the CS7000 Plus.
 *
 * A DMR direct mode transmission puts out a burst every other slot, and the
 * PA is switched on for each burst only, as the stock firmware does. The
 * HR_C6000 raises its transmit interrupt ahead of every burst it sends; from
 * there the APC voltage is ramped up, held for the burst and ramped down again
 * in steps of 50us, with the APC switch and the PA enable turned on and off at
 * set steps of the ramp. Between bursts everything is off, and a ramp always
 * comes to an end on its own.
 */

/** Longest ramp up or down, in steps. */
#define PA_RAMP_MAX_STEPS   64

/**
 * Shape of the ramp of a burst. Steps are 50us long and counted from the
 * transmit interrupt.
 */
struct paRampShape
{
    uint16_t start;                     ///< Step the ramp up starts at.
    uint16_t holdEnd;                   ///< Step the hold ends at.
    uint8_t  upLen;                     ///< Length of the ramp up.
    uint8_t  downLen;                   ///< Length of the ramp down.
    uint8_t  paOn;                      ///< Step of the ramp up turning the PA on.
    uint8_t  paOff;                     ///< Step of the ramp down turning it off.
    uint8_t  up[PA_RAMP_MAX_STEPS];     ///< APC values of the ramp up.
    uint8_t  down[PA_RAMP_MAX_STEPS];   ///< APC values of the ramp down, reversed.
};

/**
 * Build the ramp shape of a burst from its calibration, the way the stock
 * firmware does.
 *
 * @param shape: destination.
 * @param level: APC value for the output power.
 * @param start: calibrated start of the ramp up, in units of 100us.
 * @param upEnd: calibrated end of the ramp up, in units of 100us.
 * @param holdEnd: calibrated end of the hold, in units of 100us.
 * @param downEnd: calibrated end of the ramp down, in units of 100us.
 * @return false if the calibration does not describe a usable ramp.
 */
bool paRamp_build(struct paRampShape *shape, const uint8_t level,
                  const uint8_t start, const uint8_t upEnd,
                  const uint16_t holdEnd, const uint16_t downEnd);

/**
 * Set up the timer stepping the ramps. The PA is left off.
 */
void paRamp_init();

/**
 * Ramp the PA for the bursts to come, with the given shape. May be called
 * again while armed to change the shape, which applies from the next burst.
 *
 * @param shape: ramp shape, copied.
 */
void paRamp_arm(const struct paRampShape *shape);

/**
 * Stop ramping the PA: let a ramp in progress come to its end, then make sure
 * the PA is off, the APC switch released and the APC output at zero. Returns
 * once that is done.
 */
void paRamp_disarm();

/**
 * Start the ramp of a burst. To be called from the handler of the HR_C6000
 * transmit interrupt; does nothing unless armed.
 */
void paRamp_burstIsr();

#ifdef __cplusplus
}
#endif

#endif /* PARAMP_CS7000_H */
