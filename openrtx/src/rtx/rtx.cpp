/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/radio.h"
#include "interfaces/platform.h"
#include "interfaces/delays.h"
#include "hwconfig.h"
#include "core/utils.h"
#include <string.h>
#include "rtx/rtx.h"
#include "rtx/OpMode_FM.hpp"
#include "rtx/OpMode_M17.hpp"

static pthread_mutex_t   *cfgMutex;     // Mutex for incoming config messages
static const rtxStatus_t *newCnf;       // Pointer for incoming config messages
static rtxStatus_t        rtxStatus;    // RTX driver status
static rssi_t             rssi;         // Current RSSI in dBm
static bool               reinitFilter; // Flag for RSSI filter re-initialisation

static long long          watchHopTime; // Dual watch: earliest time of next hop
static rssi_t             vfoRssi[2];   // RSSI of the selected and watched VFO

/*
 * Dual watch timings, in ms. The dwell has to span at least three RTX updates
 * of 30ms each: the first RSSI sample after a retune is discarded.
 */
static const long long WATCH_DWELL = 100;   // Stay on a VFO with no signal
static const long long WATCH_HANG  = 3000;  // Stay after a signal or a TX

static OpMode  *currMode;               // Pointer to currently active opMode handler
static OpMode     noMode;               // Empty opMode handler for opmode::NONE
static OpMode_FM  fmMode;               // FM mode handler
#ifdef CONFIG_M17
static OpMode_M17 m17Mode;              // M17 mode handler
#endif

/**
 * Trade the RX parameters of the selected VFO with those of the watched one.
 * The caller has to forward the change to the radio driver.
 */
static void swapWatchParams()
{
    freq_t   freq   = rtxStatus.rxFrequency;
    uint8_t  bw     = rtxStatus.bandwidth;
    uint8_t  sql    = rtxStatus.sqlLevel;
    uint16_t toneEn = rtxStatus.rxToneEn;
    uint16_t tone   = rtxStatus.rxTone;

    rtxStatus.rxFrequency = rtxStatus.watchFrequency;
    rtxStatus.bandwidth   = rtxStatus.watchBandwidth;
    rtxStatus.sqlLevel    = rtxStatus.watchSqlLevel;
    rtxStatus.rxToneEn    = rtxStatus.watchToneEn;
    rtxStatus.rxTone      = rtxStatus.watchTone;

    rtxStatus.watchFrequency = freq;
    rtxStatus.watchBandwidth = bw;
    rtxStatus.watchSqlLevel  = sql;
    rtxStatus.watchToneEn    = toneEn;
    rtxStatus.watchTone      = tone;

    rtxStatus.onWatch = !rtxStatus.onWatch;
}

/**
 * Dual watch: with no signal, alternate the receiver between the selected VFO
 * and the watched one. A signal holds the receiver where it is. The selected
 * VFO is restored before a transmission, which never uses the watched one.
 *
 * @return true if the receiver has been retuned.
 */
static bool dualWatchUpdate()
{
    bool hop = false;
    long long now = getTick();
    bool ptt = platform_getPttStatus() && (rtxStatus.txDisable == 0);

    if((rtxStatus.dualWatch == 0) || (rtxStatus.opStatus != RX) || ptt)
    {
        // Not watching, or about to transmit: stay on the selected VFO
        hop = (rtxStatus.onWatch == 1);
        watchHopTime = now + WATCH_HANG;
        vfoRssi[1] = -127;
    }
    else
    {
        // Carrier check with the opening threshold of the FM squelch, so
        // that also a signal with the wrong tone holds the receiver.
        rssi_t squelch = sqlLevelToRssi(rtxStatus.sqlLevel);
        if(currMode->rxSquelchOpen() || (rssi > (squelch + 1)))
        {
            // The VFO left unheard reads no signal
            watchHopTime = now + WATCH_HANG;
            vfoRssi[rtxStatus.onWatch ? 0 : 1] = -127;
        }
        else if(now >= watchHopTime)
            hop = true;
    }

    if(hop)
    {
        swapWatchParams();
        radio_updateConfiguration();
        if(rtxStatus.dualWatch == 1)
            watchHopTime = now + WATCH_DWELL;
    }

    return hop;
}

void rtx_init(pthread_mutex_t *m)
{
    // Initialise mutex for configuration access
    cfgMutex = m;
    newCnf   = NULL;

    /*
     * Default initialisation for rtx status
     */
    rtxStatus.opMode        = OPMODE_NONE;
    rtxStatus.bandwidth     = BW_25;
    rtxStatus.txDisable     = 0;
    rtxStatus.opStatus      = OFF;
    rtxStatus.dualWatch     = 0;
    rtxStatus.onWatch       = 0;
    rtxStatus.rxFrequency   = 430000000;
    rtxStatus.txFrequency   = 430000000;
    rtxStatus.txPower       = 0.0f;
    rtxStatus.sqlLevel      = 1;
    rtxStatus.rxToneEn      = 0;
    rtxStatus.rxTone        = 0;
    rtxStatus.txToneEn      = 0;
    rtxStatus.txTone        = 0;
    rtxStatus.invertRxPhase = false;
    rtxStatus.lsfOk         = false;
    rtxStatus.M17_src[0]    = '\0';
    rtxStatus.M17_dst[0]    = '\0';
    rtxStatus.M17_link[0]   = '\0';
    rtxStatus.M17_refl[0]   = '\0';
    rtxStatus.M17_meta_text[0] = '\0';
    currMode = &noMode;

    /*
     * Initialise low-level platform-specific driver
     */
    radio_init(&rtxStatus);
    radio_updateConfiguration();

    /*
     * Initial value for RSSI filter
     */
    rssi         = radio_getRssi();
    reinitFilter = false;
    vfoRssi[0]   = rssi;
    vfoRssi[1]   = -127;
}

void rtx_terminate()
{
    rtxStatus.opStatus = OFF;
    rtxStatus.opMode   = OPMODE_NONE;
    currMode->disable();
    radio_terminate();
}

void rtx_configure(const rtxStatus_t *cfg)
{
    /*
     * NOTE: an incoming configuration may overwrite a preceding one not yet
     * read by the radio task. This mechanism ensures that the radio driver
     * always gets the most recent configuration.
     */

    pthread_mutex_lock(cfgMutex);
    newCnf = cfg;
    pthread_mutex_unlock(cfgMutex);
}

rtxStatus_t rtx_getCurrentStatus()
{
    return rtxStatus;
}

void rtx_task()
{
    // Check if there is a pending new configuration and, in case, read it.
    bool reconfigure = false;
    if(pthread_mutex_trylock(cfgMutex) == 0)
    {
        if(newCnf != NULL)
        {
            // Copy new configuration and override opStatus flags
            uint8_t tmp = rtxStatus.opStatus;
            bool onWatch = (rtxStatus.onWatch == 1);
            freq_t tuned = rtxStatus.rxFrequency;
            memcpy(&rtxStatus, newCnf, sizeof(rtxStatus_t));
            rtxStatus.opStatus = tmp;

            // Dual watch: stay on the watched VFO if the receiver is there
            rtxStatus.onWatch = 0;
            if(onWatch && (rtxStatus.dualWatch == 1) &&
               (tuned == rtxStatus.watchFrequency))
                swapWatchParams();

            reconfigure = true;
            newCnf = NULL;
        }

        pthread_mutex_unlock(cfgMutex);
    }

    if(reconfigure)
    {
        // Force TX and RX tone squelch to off for OpModes different from FM.
        if(rtxStatus.opMode != OPMODE_FM)
        {
            rtxStatus.txToneEn = 0;
            rtxStatus.rxToneEn = 0;
        }

        /*
         * Handle change of opMode:
         * - deactivate current opMode and switch operating status to "OFF";
         * - update pointer to current mode handler to the OpMode object for the
         *   selected mode;
         * - enable the new mode handler
         */
        if(currMode->getID() != rtxStatus.opMode)
        {
            // Forward opMode change also to radio driver
            radio_setOpmode(static_cast< enum opmode >(rtxStatus.opMode));

            currMode->disable();
            rtxStatus.opStatus = OFF;

            switch(rtxStatus.opMode)
            {
                case OPMODE_NONE: currMode = &noMode;  break;
                case OPMODE_FM:   currMode = &fmMode;  break;
                #ifdef CONFIG_M17
                case OPMODE_M17:  currMode = &m17Mode; break;
                #endif
                default:   currMode = &noMode;
            }

            currMode->enable();
        }

        // Tell radio driver that there was a change in its configuration.
        radio_updateConfiguration();
    }

    // A dual watch hop retunes the receiver like a new configuration does
    bool retuned = reconfigure;
    if(dualWatchUpdate())
    {
        retuned = true;
        reinitFilter = true;

        // The level of the VFO just left must not open the squelch of this one
        rssi = -127;
    }

    /*
     * RSSI update block, run only when radio is in RX mode.
     *
     * RSSI value is passed through a filter with a time constant of 60ms
     * (cut-off frequency of 15Hz) at an update rate of 33.3Hz.
     *
     * The low pass filter skips an update step if a new configuration has
     * just been applied. This is a workaround for the AT1846S returning a
     * full-scale RSSI value immediately after one of its parameters changed,
     * thus causing the squelch to open briefly.
     *
     * Also, the RSSI filter is re-initialised every time radio stage is
     * switched back from TX/OFF to RX. This provides a workaround for some
     * radios reporting a full-scale RSSI value when transmitting.
     */
    if(rtxStatus.opStatus == RX)
    {

        if(!retuned)
        {
            if(!reinitFilter)
            {
                /*
                 * Filter RSSI value using 15.16 fixed point math. Equivalent
                 * floating point code is: rssi = 0.74*radio_getRssi() + 0.26*rssi
                 */
                int32_t filt_rssi = radio_getRssi() * 0xBD70    // 0.74 * radio_getRssi
                                  + rssi            * 0x428F;   // 0.26 * rssi
                rssi = (filt_rssi + 32768) >> 16;               // Round to nearest
            }
            else
            {
                rssi = radio_getRssi();
                reinitFilter = false;
            }

            vfoRssi[rtxStatus.onWatch] = rssi;
        }
    }
    else
    {
        // Reinit required if current operating status is TX or OFF
        reinitFilter = true;
    }

    /*
     * Forward the periodic update step to the currently active opMode handler.
     * Call is placed after RSSI update to allow handler's code have a fresh
     * version of the RSSI level.
     */
    currMode->update(&rtxStatus, reconfigure);
}

rssi_t rtx_getRssi()
{
    return rssi;
}

rssi_t rtx_getVfoRssi(const bool watched)
{
    return vfoRssi[watched ? 1 : 0];
}

bool rtx_rxSquelchOpen()
{
    return currMode->rxSquelchOpen();
}
