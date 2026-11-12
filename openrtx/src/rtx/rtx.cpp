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
static bool               watchPeek;    // Dual watch: peeking at the other VFO
static rssi_t             vfoRssi[2];   // RSSI of the selected and watched VFO

/*
 * Dual watch timings, in ms. The FM dwell has to span at least three RTX
 * updates of 30ms each: the first RSSI sample after a retune is discarded.
 * The M17 dwell covers the restart of the baseband sampling and the search
 * for two sync words, 40ms apart.
 */
static const long long WATCH_DWELL_FM  = 100;   // Stay on a quiet FM VFO
static const long long WATCH_DWELL_M17 = 240;   // Stay on a quiet M17 VFO
static const long long WATCH_CONFIRM   = 500;   // Stay after a carrier is found
static const long long WATCH_HANG      = 3000;  // Stay after a signal or a TX

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
    uint8_t  mode   = rtxStatus.opMode;
    uint8_t  can    = rtxStatus.m17_can;
    freq_t   freq   = rtxStatus.rxFrequency;
    uint8_t  bw     = rtxStatus.bandwidth;
    uint8_t  sql    = rtxStatus.sqlLevel;
    uint16_t toneEn = rtxStatus.rxToneEn;
    uint16_t tone   = rtxStatus.rxTone;

    rtxStatus.opMode      = rtxStatus.watchOpMode;
    rtxStatus.m17_can     = rtxStatus.watchCan;
    rtxStatus.rxFrequency = rtxStatus.watchFrequency;
    rtxStatus.bandwidth   = rtxStatus.watchBandwidth;
    rtxStatus.sqlLevel    = rtxStatus.watchSqlLevel;
    rtxStatus.rxToneEn    = rtxStatus.watchToneEn;
    rtxStatus.rxTone      = rtxStatus.watchTone;

    rtxStatus.watchOpMode    = mode;
    rtxStatus.watchCan       = can;
    rtxStatus.watchFrequency = freq;
    rtxStatus.watchBandwidth = bw;
    rtxStatus.watchSqlLevel  = sql;
    rtxStatus.watchToneEn    = toneEn;
    rtxStatus.watchTone      = tone;

    rtxStatus.onWatch = !rtxStatus.onWatch;
}

/**
 * Dual watch: start or end a peek. The receiver goes to the frequency of the
 * other VFO, or comes back from it, while the handler of the current mode
 * stays in place with its reception suspended. The caller has to forward the
 * change to the radio driver.
 *
 * @param peek: true to start a peek, false to end it.
 */
static void setWatchPeek(const bool peek)
{
    freq_t freq = rtxStatus.rxFrequency;

    rtxStatus.rxFrequency    = rtxStatus.watchFrequency;
    rtxStatus.watchFrequency = freq;

    currMode->rxSuspend(peek);
    watchPeek = peek;
}

/**
 * Make the handler of rtxStatus.opMode the current one, if it is not already.
 * The handler taking over starts from the OFF operating status.
 */
static void switchOpMode()
{
    if(currMode->getID() == rtxStatus.opMode)
        return;

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

/**
 * Dual watch: with no signal, alternate the receiver between the selected VFO
 * and the watched one, each in its own operating mode. A signal holds the
 * receiver where it is. The selected VFO is restored before a transmission,
 * which never uses the watched one.
 *
 * An FM VFO shows a signal by its carrier, a mode demodulated by the MCU only
 * to its demodulator. Between the two the receiver rests on the latter and
 * peeks at the FM one: a short retune for an RSSI sample, see
 * dualWatchPeekEnd(), with no change of handler.
 *
 * @return true if the receiver has been retuned.
 */
static bool dualWatchUpdate()
{
    bool hop = false;
    bool peek = false;
    long long now = getTick();
    bool ptt = platform_getPttStatus() && (rtxStatus.txDisable == 0);

    if((rtxStatus.dualWatch == 0) || (rtxStatus.opStatus == TX) || ptt)
    {
        // Not watching, or about to transmit: stay on the selected VFO
        if(watchPeek)
        {
            setWatchPeek(false);
            radio_updateConfiguration();
            peek = true;
        }

        hop = (rtxStatus.onWatch == 1);
        watchHopTime = now + WATCH_HANG;
        vfoRssi[1] = -127;
    }
    else if((rtxStatus.opStatus == RX) && (watchPeek == false))
    {
        // FM carrier check with the opening threshold of the squelch, so
        // that also a signal with the wrong tone holds the receiver.
        rssi_t squelch = sqlLevelToRssi(rtxStatus.sqlLevel);
        bool carrier = (rtxStatus.opMode == OPMODE_FM)
                    && (rssi > (squelch + 1));

        if(currMode->rxSquelchOpen() || carrier)
        {
            // The VFO left unheard reads no signal
            watchHopTime = now + WATCH_HANG;
            vfoRssi[rtxStatus.onWatch ? 0 : 1] = -127;
        }
        else if((now >= watchHopTime) && (currMode->rxActive() == false))
        {
            if((rtxStatus.opMode != OPMODE_FM) &&
               (rtxStatus.watchOpMode == OPMODE_FM))
            {
                setWatchPeek(true);
                radio_updateConfiguration();
                peek = true;
            }
            else
                hop = true;
        }
    }

    if(hop)
    {
        OpMode *prevMode = currMode;

        swapWatchParams();
        switchOpMode();
        radio_updateConfiguration();

        // A hop inside the same mode keeps the handler: restart its reception
        if(currMode == prevMode)
        {
            currMode->rxSuspend(true);
            currMode->rxSuspend(false);
        }

        if(rtxStatus.dualWatch == 1)
            watchHopTime = now + ((rtxStatus.opMode == OPMODE_M17)
                                  ? WATCH_DWELL_M17 : WATCH_DWELL_FM);
    }

    return hop || peek;
}

/**
 * Dual watch: end a peek once the RSSI of the other VFO has been sampled. A
 * carrier above its squelch takes the receiver there, with the handler of its
 * mode; otherwise the receiver goes back and the reception resumes.
 */
static void dualWatchPeekEnd()
{
    long long now = getTick();
    rssi_t squelch = sqlLevelToRssi(rtxStatus.watchSqlLevel);
    bool carrier = (rssi > (squelch + 1));

    vfoRssi[rtxStatus.onWatch ? 0 : 1] = rssi;
    setWatchPeek(false);

    if(carrier)
    {
        swapWatchParams();
        switchOpMode();
        watchHopTime = now + WATCH_CONFIRM;
    }
    else
        watchHopTime = now + WATCH_DWELL_M17;

    radio_updateConfiguration();
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
    rtxStatus.watchOpMode   = OPMODE_NONE;
    watchPeek               = false;
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
            // A peek ends with the configuration it belongs to
            if(watchPeek)
                setWatchPeek(false);

            bool onWatch = (rtxStatus.onWatch == 1);
            freq_t tuned = rtxStatus.rxFrequency;
            uint8_t tunedMode = rtxStatus.opMode;
            memcpy(&rtxStatus, newCnf, sizeof(rtxStatus_t));
            rtxStatus.opStatus = tmp;

            // Force TX and RX tone squelch off for OpModes other than FM
            if(rtxStatus.opMode != OPMODE_FM)
            {
                rtxStatus.txToneEn = 0;
                rtxStatus.rxToneEn = 0;
            }

            // Dual watch: stay on the watched VFO if the receiver is there
            rtxStatus.onWatch = 0;
            if(onWatch && (rtxStatus.dualWatch == 1) &&
               (tuned == rtxStatus.watchFrequency) &&
               (tunedMode == rtxStatus.watchOpMode))
                swapWatchParams();

            reconfigure = true;
            newCnf = NULL;
        }

        pthread_mutex_unlock(cfgMutex);
    }

    if(reconfigure)
    {
        /*
         * Handle change of opMode:
         * - deactivate current opMode and switch operating status to "OFF";
         * - update pointer to current mode handler to the OpMode object for the
         *   selected mode;
         * - enable the new mode handler
         */
        switchOpMode();

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

            if(watchPeek)
            {
                // The sample belongs to the other VFO, and to no handler
                dualWatchPeekEnd();
                reinitFilter = true;
                rssi = -127;
            }
            else
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
