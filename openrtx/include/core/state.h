/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef STATE_H
#define STATE_H

#include "core/datatypes.h"
#include "core/settings.h"
#include <pthread.h>
#include <stdbool.h>
#include "core/cps.h"
#include "core/gps.h"
#include "core/tuner.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Part of this structure has been commented because the corresponding
 * functionality is not yet implemented.
 * Uncomment once the related feature is ready
 */
typedef struct {
    uint8_t devStatus;
    datetime_t time;
    uint16_t v_bat;
    uint8_t charge;
    rssi_t rssi;     // RSSI of the selected VFO
    rssi_t rssi_alt; // RSSI of the VFO not selected
    uint8_t volume;

    uint8_t ui_screen;
    uint8_t tuner_mode;

    uint16_t channel_index;
    bool bank_enabled;     // Memory mode browses a bank, not all channels
    bool bank_enabled_alt; // Same, for the VFO not selected
    struct tuner tuner;     // Selected VFO
    struct tuner tuner_alt; // VFO not selected

    uint8_t rtxStatus;
    bool tone_enabled;

    bool emergency;
    struct settings settings;
    gps_t gps_data;
    bool gps_set_time;
    bool gpsDetected;
    bool backup_eflash;
    bool restore_eflash;
    bool txDisable;
    uint8_t step_index;
} state_t;

extern const uint32_t freq_steps[];
extern const size_t n_freq_steps;

enum TunerMode {
    VFO = 0,
    CH,
    SCAN,
    CHSCAN,
};

enum RtxStatus {
    RTX_OFF = 0,
    RTX_RX,
    RTX_TX,
};

enum DeviceStatus {
    PWROFF = 0,
    STARTUP,
    RUNNING,
    DATATRANSFER,
    SHUTDOWN,
};

extern state_t state;
extern pthread_mutex_t state_mutex;

/**
 * Initialise radio state mutex and radio state variable, reading the
 * informations from device drivers.
 */
void state_init();

/**
 * Terminate the radio state saving persistent settings to flash and destroy
 * the state mutex.
 */
void state_terminate();

/**
 * Update radio state fetching data from device drivers.
 */
void state_task();

/**
 * Reset the fields of radio state containing user settings and VFO channel.
 */
void state_resetSettingsAndVfo();

/**
 * Put both tuners back in the mode they were saved in, loading their channels
 * from the codeplug. To be called once the codeplug is open.
 */
void state_loadTuners();

/**
 * Select the other VFO: the selected VFO and the one not selected trade places.
 */
void state_swapVfo();

/**
 * Save both VFOs to nonvolatile memory, VFO A first, whichever is selected.
 *
 * @return 0 if successful, a negative error code otherwise.
 */
int state_saveVfos();

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* STATE_H */
