/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "hwconfig.h"
#include <pthread.h>
#include "core/ui.h"
#include "core/state.h"
#include "core/threads.h"
#include "core/graphics.h"
#include "interfaces/platform.h"
#include "interfaces/delays.h"
#include "interfaces/radio.h"
#include "core/event.h"
#include "rtx/rtx.h"
#include <string.h>
#include "core/utils.h"
#include "core/input.h"
#include "core/backup.h"
#include "core/gps.h"
#include "core/voicePrompts.h"
#include "core/messages.h"
#include "core/m17_sms.h"
#include "core/aprs_msg.h"
#include "core/aprs_traffic.h"
#include "core/stations.h"
#include "core/aprs_beacon.h"
#include "core/beeps.h"

#if defined(PLATFORM_TTWRPLUS)
#include "pmu.h"
#endif

/* Mutex for concurrent access to RTX state variable */
pthread_mutex_t rtx_mutex;

#if defined(CONFIG_APRS)
/**
 * \internal Assemble the beacon configuration and current position from the
 * radio state and run one beacon tick. Called about once a second while APRS
 * or M17 is the active mode; the scheduler decides whether anything is
 * transmitted.
 */
static void beacon_service(const state_t *st, uint32_t nowSec)
{
    static struct beaconState bstate;
    static bool     inited  = false;
    static uint32_t lastSec = 0;

    if(!inited)
    {
        beacon_reset(&bstate);
        inited  = true;
        lastSec = nowSec;
        return;
    }

    if(nowSec == lastSec)   // at most once per second
        return;
    lastSec = nowSec;

    // Each carrier has its own switch; the schedule is shared, and starts
    // over when the carrier changes.
    static uint8_t lastCarrier = OPMODE_APRS;
    uint8_t carrier = st->channel.mode;
    if(carrier != lastCarrier)
    {
        beacon_reset(&bstate);
        lastCarrier = carrier;
    }
    if((carrier == OPMODE_M17) ? !st->settings.m17_beacon
                               : !st->settings.aprs_beacon)
        return;

    struct aprsBeaconParams params;
    params.sched.method       = (enum beaconMethod) st->settings.beacon_method;
    params.sched.minInterval  = st->settings.beacon_interval;
    params.sched.maxInterval  = st->settings.beacon_maxInterval;
    params.sched.errThreshold = st->settings.beacon_errThreshold;
    params.callsign           = st->settings.aprs_callsign;
    params.path               = st->settings.aprs_path;
    params.symbolTable        = st->settings.aprs_symTable;
    params.symbolCode         = st->settings.aprs_symCode;
    params.comment            = st->settings.beacon_comment;
    params.sendCourseSpeed    = st->settings.beacon_send_cs;
    params.sendAltitude       = st->settings.beacon_send_alt;
    params.carrier            = carrier;
    params.m17Callsign        = st->settings.callsign;

    struct beaconFix fix;
    if(st->settings.beacon_pos_fixed)
    {
        // A fixed position has no course, speed or altitude to report, and
        // "/A=000000" would claim sea level.
        params.sendCourseSpeed = false;
        params.sendAltitude    = false;

        fix.valid     = true;
        fix.latitude  = st->settings.beacon_fixedLat;
        fix.longitude = st->settings.beacon_fixedLon;
        fix.course    = 0;
        fix.speed     = 0;
        fix.altitude  = 0;
    }
    else
    {
        // A fix the receiver stopped confirming is not a position to send.
        const gps_t *g = &st->gps_data;
        uint32_t fixAge = (uint32_t) getTick() - g->fix_tick;
        fix.valid     = (st->settings.gps_enabled != 0)
                        && (g->fix_quality != FIX_QUALITY_NO_FIX)
                        && (g->fix_quality != FIX_QUALITY_ESTIMATED)
                        && (g->fix_tick != 0)
                        && (fixAge < BEACON_FIX_MAX_AGE_MS);
        fix.latitude  = g->latitude;
        fix.longitude = g->longitude;
        fix.course    = (uint16_t) g->tmg_true;
        fix.speed     = g->speed;
        fix.altitude  = g->altitude;
    }

    aprsBeacon_tick(&params, &fix, &bstate, nowSec);
}
#endif

#if defined(CONFIG_M17)
/**
 * \internal Put the station of an M17 transmission in the station table,
 * with its position when the stream carried GNSS metadata. Called once per
 * UI loop; the status copy is the hand-off from the RTX thread.
 */
static void m17_stationService(uint32_t nowSec)
{
    static bool    wasHeard = false;
    static uint8_t lastSeq  = 0;

    const rtxStatus_t *st = rtx_getStatus();
    bool heard = (st->opMode == OPMODE_M17) && st->lsfOk
              && (st->M17_src[0] != '\0');
    bool fresh = heard && (st->M17_gnss.seq != lastSeq);

    if(heard && (!wasHeard || fresh))
    {
        struct station *s = stations_update(st->M17_src, STATION_CARRIER_M17,
                                            nowSec);
        if((s != NULL) && fresh)
        {
            s->latitude  = st->M17_gnss.latitude;
            s->longitude = st->M17_gnss.longitude;
            s->valid     = STATION_HAS_POSITION;
            // Altitude and velocity flags come straight from the sender
            if(st->M17_gnss.valid & 2)
            {
                s->altitude = st->M17_gnss.altitude;
                s->valid   |= STATION_HAS_ALTITUDE;
            }
            if(st->M17_gnss.valid & 4)
            {
                s->speed  = st->M17_gnss.speed;
                s->course = (int16_t)(st->M17_gnss.bearing % 360);
                s->valid |= STATION_HAS_VELOCITY;
            }
        }
    }

    if(fresh)
        lastSeq = st->M17_gnss.seq;
    wasHeard = heard;
}
#endif

/**
 * \internal Thread managing user input and UI
 */
void *ui_threadFunc(void *arg)
{
    (void) arg;

    kbd_msg_t   kbd_msg;
    rtxStatus_t rtx_cfg = { 0 };
    bool        sync_rtx = true;
    long long   time     = 0;

    #if defined(CONFIG_MESSAGES)
    messages_init();
    #endif

    #if defined(CONFIG_M17_SMS)
    messages_registerSource(&m17_sms_ops);
    #endif
    #if defined(CONFIG_APRS)
    messages_registerSource(&aprs_msg_ops);
    aprsTraffic_init();
    #endif
    stations_init();

    // Load initial state and update the UI
    ui_saveState();
    ui_updateGUI();

    // Keep the splash screen for one second  before rendering the new UI screen
    sleepFor(1u, 0u);
    gfx_render();

    while(state.devStatus != SHUTDOWN)
    {
        time = getTick();

        if(input_scanKeyboard(&kbd_msg))
        {
            ui_pushEvent(EVENT_KBD, kbd_msg.value);
        }

        pthread_mutex_lock(&state_mutex);   // Lock r/w access to radio state
        ui_updateFSM(&sync_rtx);            // Update UI FSM
        ui_saveState();                     // Save local state copy
        pthread_mutex_unlock(&state_mutex); // Unlock r/w access to radio state

        vp_tick();                           // continue playing voice prompts in progress if any.

        #if defined(CONFIG_MESSAGES)
        if(messages_task(state.channel.mode) > 0)
            vp_beep(BEEP_NEW_MESSAGE, LONG_BEEP);
        #endif

        #if defined(CONFIG_APRS)
        // Position beacons go out in APRS, or as APRS in M17 packet mode.
        if((state.channel.mode == OPMODE_APRS) ||
           (state.channel.mode == OPMODE_M17))
            beacon_service(&state, (uint32_t)(time / 1000));

        // An emergency heard on the air keeps sounding until it is seen.
        {
            static long long lastAlarm = 0;
            if(aprs_msg_emergency(NULL, 0, NULL) && ((time - lastAlarm) >= 5000))
            {
                vp_beep(BEEP_NEW_MESSAGE, LONG_BEEP);
                lastAlarm = time;
            }
        }
        #endif

        #if defined(CONFIG_M17)
        m17_stationService((uint32_t)(time / 1000));
        #endif

        // If synchronization needed take mutex and update RTX configuration
        if(sync_rtx)
        {
            pthread_mutex_lock(&rtx_mutex);
            rtx_cfg.opMode      = state.channel.mode;
            rtx_cfg.bandwidth   = state.channel.bandwidth;
            rtx_cfg.rxFrequency = state.channel.rx_frequency;
            rtx_cfg.txFrequency = state.channel.tx_frequency;
            rtx_cfg.txPower     = state.channel.power;
            rtx_cfg.sqlLevel    = state.settings.sqlLevel;
            rtx_cfg.rxToneEn    = state.channel.fm.rxToneEn;
            rtx_cfg.rxTone      = ctcss_tone[state.channel.fm.rxTone];
            rtx_cfg.txToneEn    = state.channel.fm.txToneEn;
            rtx_cfg.txTone      = ctcss_tone[state.channel.fm.txTone];
            rtx_cfg.toneEn      = state.tone_enabled;

            // Hard channel TX lockout: rx-only channels block all transmission.
            // PTT gate: additionally blocked when the UI is outside VFO/MEM screens.
            rtx_cfg.txDisable  = state.channel.rx_only;
            rtx_cfg.pttDisable = state.channel.rx_only || state.pttDisable;

            // Copy new M17 CAN, source and destination addresses
            rtx_cfg.can = state.settings.m17_can;
            rtx_cfg.canRxEn = state.settings.m17_can_rx;
            strncpy(rtx_cfg.source_address,      state.settings.callsign, 10);
            strncpy(rtx_cfg.destination_address, state.settings.m17_dest, 10);
            #if defined(CONFIG_APRS)
            rtx_cfg.aprs_digi.mode       = state.settings.aprs_digiMode;
            rtx_cfg.aprs_digi.delay      = state.settings.aprs_digiDelay;
            rtx_cfg.aprs_digi.maxHops    = state.settings.aprs_digiHops;
            rtx_cfg.aprs_digi.directOnly = state.settings.aprs_digiDirect;
            strncpy(rtx_cfg.aprs_digi.call,  state.settings.aprs_callsign,  10);
            strncpy(rtx_cfg.aprs_digi.alias, state.settings.aprs_digiAlias, 10);
            #endif

            pthread_mutex_unlock(&rtx_mutex);

            rtx_configure(&rtx_cfg);
            sync_rtx = false;
        }

        // Update UI and render on screen, if necessary
        if(ui_updateGUI() == true)
        {
            gfx_render();
        }

        // 40Hz update rate for keyboard and UI
        time += 25;
        sleepUntil(time);
    }

    ui_terminate();
    gfx_terminate();

    #if defined(CONFIG_MESSAGES)
    messages_terminate();
    #endif

    return NULL;
}

/**
 * \internal Thread managing the device and update the global state variable.
 */
void *main_thread(void *arg)
{
    (void) arg;

    long long time = 0;

    #if defined(CONFIG_GPS)
    const struct gpsDevice *gps = platform_initGps();
    if(gps != NULL)
        state.gpsDetected = true;
    #endif

    while(state.devStatus != SHUTDOWN)
    {
        time = getTick();

        #if defined(PLATFORM_TTWRPLUS)
        pmu_handleIRQ();
        #endif

        // Check if power off is requested
        pthread_mutex_lock(&state_mutex);
        if(platform_pwrButtonStatus() == false)
            state.devStatus = SHUTDOWN;
        pthread_mutex_unlock(&state_mutex);

        // Run GPS task
        #if defined(CONFIG_GPS)
        gps_task(gps);
        #endif

        // Run state update task
        state_task();

        // Run this loop once every 5ms
        time += 5;
        sleepUntil(time);
    }

    return NULL;
}

/**
 * \internal Thread for RTX management.
 */
void *rtx_threadFunc(void *arg)
{
    (void) arg;

    rtx_init(&rtx_mutex);

    while(state.devStatus == RUNNING)
    {
        rtx_task();
    }

    rtx_terminate();

    return NULL;
}

/**
 * \internal This function creates all the system tasks and mutexes.
 */
void create_threads()
{
    // Create RTX state mutex
    pthread_mutex_init(&rtx_mutex, NULL);

    // Create rtx radio thread
    pthread_attr_t rtx_attr;
    pthread_attr_init(&rtx_attr);

    #ifndef __ZEPHYR__
    pthread_attr_setstacksize(&rtx_attr, RTX_THREAD_STKSIZE);
    #else
    void *rtx_thread_stack = malloc(RTX_THREAD_STKSIZE * sizeof(uint8_t));
    pthread_attr_setstack(&rtx_attr, rtx_thread_stack, RTX_THREAD_STKSIZE);
    #endif

    #ifdef _MIOSIX
    // Max priority for RTX thread when running with miosix rtos
    struct sched_param param;
    param.sched_priority = THREAD_PRIO_HIGH;
    pthread_attr_setschedparam(&rtx_attr, &param);
    #endif

    pthread_t rtx_thread;
    pthread_create(&rtx_thread, &rtx_attr, rtx_threadFunc, NULL);

    // Create UI thread
    pthread_attr_t ui_attr;
    pthread_attr_init(&ui_attr);

    #ifndef __ZEPHYR__
    pthread_attr_setstacksize(&ui_attr, UI_THREAD_STKSIZE);
    #else
    void *ui_thread_stack = malloc(UI_THREAD_STKSIZE * sizeof(uint8_t));
    pthread_attr_setstack(&ui_attr, ui_thread_stack, UI_THREAD_STKSIZE);
    #endif

    pthread_t ui_thread;
    pthread_create(&ui_thread, &ui_attr, ui_threadFunc, NULL);
}
