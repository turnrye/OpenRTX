/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef VFO_H
#define VFO_H

#include <stdint.h>
#include "core/tuner.h"

/**
 * Number of VFOs stored in nonvolatile memory, saved and loaded together.
 */
#define VFO_NUM 2

/**
 * Initialize VFO data storage.
 *
 * @param nvm_dev: NVM device number for VFO storage
 * @param nvm_part: partition number for VFO storage
 */
void vfo_init(const uint32_t nvm_dev, const uint32_t nvm_part);

/**
 * Load the data of all the VFOs from nonvolatile memory.
 *
 * On failure, the VFO data is left untouched.
 *
 * @param vfos: array of VFO_NUM tuners receiving the VFO data.
 * @return 0 if successful, a negative error code otherwise
 */
int vfo_load(struct tuner_core vfos[VFO_NUM]);

/**
 * Save the data of all the VFOs to nonvolatile memory.
 * Will not perform any actual write if VFO data haven't changed.
 *
 * @param vfos: array of VFO_NUM tuners holding the VFO data.
 * @return 0 if successful, a negative error code otherwise
 */
int vfo_save(const struct tuner_core vfos[VFO_NUM]);

#endif
