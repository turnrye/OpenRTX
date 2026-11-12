/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef VFO_H
#define VFO_H

#include <stdint.h>

struct tuner_core;

/**
 * Initialize VFO data storage.
 *
 * @param nvm_dev: NVM device number for VFO storage
 * @param nvm_part: partition number for VFO storage
 */
void vfo_init(const uint32_t nvm_dev, const uint32_t nvm_part);

/**
 * Load device VFO data from nonvolatile memory.
 *
 * On failure, the VFO data is left untouched.
 *
 * @param vfo: pointer to the tuner data receiving the VFO.
 * @return 0 if successful, a negative error code otherwise
 */
int vfo_load(struct tuner_core *vfo);

/**
 * Save VFO data to nonvolatile memory.
 * Will not perform any actual write if VFO data haven't changed.
 *
 * @param vfo: pointer to the tuner data holding the VFO.
 * @return 0 if successful, a negative error code otherwise
 */
int vfo_save(const struct tuner_core *vfo);

#endif
