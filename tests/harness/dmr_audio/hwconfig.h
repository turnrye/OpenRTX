/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HWCONFIG_H
#define HWCONFIG_H

/*
 * The DMR voice pipeline test's radio: DMR enabled and a microphone gate with
 * thresholds, so that frames without voice go out as the silence frame.
 */
#define CONFIG_DMR
#define CONFIG_MIC_GATE_OPEN 1000
#define CONFIG_MIC_GATE_CLOSE 500

#endif
