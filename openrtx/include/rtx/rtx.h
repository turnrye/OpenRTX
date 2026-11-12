/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RTX_H
#define RTX_H

#include "core/datatypes.h"
#include <stdint.h>
#include "core/cps.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint8_t opMode;         /**< Operating mode (FM, DMR, ...) */

    uint8_t bandwidth : 2,  /**< Channel bandwidth             */
            txDisable : 1,  /**< Disable TX operation          */
            scan      : 1,  /**< Scan enabled                  */
            opStatus  : 2,  /**< Operating status (OFF, ...)   */
            dualWatch : 1,  /**< Dual watch enabled            */
            onWatch   : 1;  /**< Receiver is on the watched VFO */

    freq_t rxFrequency;     /**< RX frequency, in Hz           */
    freq_t txFrequency;     /**< TX frequency, in Hz           */

    uint32_t txPower;       /**< TX power, in mW               */
    uint8_t  sqlLevel;      /**< Squelch opening level         */

    uint16_t rxToneEn : 1,  /**< RX CTC/DCS tone enable        */
             rxTone   : 15; /**< RX CTC/DCS tone               */

    uint16_t txToneEn : 1,  /**< TX CTC/DCS tone enable        */
             txTone   : 15; /**< TX CTC/DCS tone               */

    bool     toneEn;

    uint8_t  m17_can   : 4;  /**< M17 Channel Access Number     */
    bool     canRxEn;       /**< M17 Check CAN on RX           */

    /*
     * Dual watch: RX parameters of the watched VFO. While onWatch is set they
     * trade places with opMode, rxFrequency, bandwidth, sqlLevel, rxToneEn,
     * rxTone and m17_can, so that these always describe what the receiver is
     * tuned to.
     */
    uint8_t  watchOpMode : 4,   /**< Watched operating mode        */
             watchCan    : 4;   /**< Watched M17 CAN               */
    freq_t   watchFrequency;    /**< Watched RX frequency, in Hz   */
    uint8_t  watchBandwidth;    /**< Watched channel bandwidth     */
    uint8_t  watchSqlLevel;     /**< Watched squelch opening level */
    uint16_t watchToneEn : 1,   /**< Watched RX CTC/DCS tone enable */
             watchTone   : 15;  /**< Watched RX CTC/DCS tone       */

    char     source_address[10];       /**< M17 call source address    */
    char     destination_address[10];  /**< M17 call routing address   */
    bool     invertRxPhase;            /**< M17 RX phase inversion     */
    bool     lsfOk;                    /**  M17 LSF is valid           */
    char     M17_dst[10];              /**  M17 LSF destination        */
    char     M17_src[10];              /**  M17 LSF source             */
    char     M17_link[10];             /**  M17 LSF traffic originator */
    char     M17_refl[10];             /**  M17 LSF reflector module   */
    char     M17_meta_text[53];        /**< M17 Meta Text              */
}
rtxStatus_t;

/**
 * \enum bandwidth Enumeration type defining the current rtx bandwidth.
 */
enum bandwidth
{
    BW_12_5 = 0,    /**< 12.5kHz bandwidth */
    BW_25   = 1     /**< 25kHz bandwidth   */
};

/**
 * \enum opmode Enumeration type defining the current rtx operating mode.
 */
enum opmode
{
    OPMODE_NONE = 0,        /**< No opMode selected */
    OPMODE_FM   = 1,        /**< Analog FM          */
    OPMODE_DMR  = 2,        /**< DMR                */
    OPMODE_M17  = 3         /**< M17                */
};

/**
 * \enum opstatus Enumeration type defining the current rtx operating status.
 */
enum opstatus
{
    OFF = 0,        /**< OFF          */
    RX  = 1,        /**< Receiving    */
    TX  = 2         /**< Transmitting */
};


/**
 * Initialise rtx stage.
 * @param m: pointer to the mutex protecting the shared configuration data
 * structure.
 */
void rtx_init(pthread_mutex_t *m);

/**
 * Shut down rtx stage
 */
void rtx_terminate();

/**
 * Post a new RTX configuration on the internal message queue. Data structure
 * \b must be protected by the same mutex whose pointer has been passed as a
 * parameter to rtx_init(). This driver only copies its content into the internal
 * data structure, eventual garbage collection has to be performed by caller.
 * @param cfg: pointer to a structure containing the new RTX configuration.
 */
void rtx_configure(const rtxStatus_t *cfg);

/**
 * Obtain a copy of the RTX driver's internal status data structure.
 * @return copy of the RTX driver's internal status data structure.
 */
rtxStatus_t rtx_getCurrentStatus();

/**
 * High-level code is in charge of calling this function periodically, since it
 * contains all the RTX management functionalities.
 */
void rtx_task();

/**
 * Get current RSSI in dBm.
 * @return RSSI value in dBm.
 */
rssi_t rtx_getRssi();

/**
 * Get the RSSI last measured on one of the two VFOs. With dual watch off the
 * selected VFO reads as rtx_getRssi(). A VFO the receiver is not listening
 * to reads -127dBm.
 * @param watched: true for the watched VFO, false for the selected one.
 * @return RSSI value in dBm.
 */
rssi_t rtx_getVfoRssi(const bool watched);

/**
 * Get current status of the RX squelch. This function is thread-safe and can
 * be called also from threads other than the one running the RTX task.
 * @return true if RX squelch is open.
 */
bool rtx_rxSquelchOpen();

#ifdef __cplusplus
}
#endif

#endif /* RTX_H */
