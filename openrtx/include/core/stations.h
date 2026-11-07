/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef STATIONS_H
#define STATIONS_H

#include "protocols/APRS/packet.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Table of stations heard, one record per callsign, latest state wins:
 * where a station is, what it is, when it was last heard. Any carrier can
 * feed it; the units are those of gps_t, so a GPS fix or an APRS or M17
 * position lands here without a conversion of its own. Kept newest first.
 * UI thread only.
 */

/** Longest callsign or object name, with the NUL. */
#define STATION_CALL_LEN 10

/** Which fields of a record hold a value. */
enum stationValid {
    STATION_HAS_POSITION = 1,
    STATION_HAS_ALTITUDE = 2,
    STATION_HAS_VELOCITY = 4, /**< Course and speed together. */
    STATION_HAS_SYMBOL = 8,
    STATION_POSITION_AMBIGUOUS = 16,
    STATION_EMERGENCY = 32, /**< Its last packet raised an emergency. */
    STATION_HAS_FREQ = 64,  /**< It announced a frequency to tune to.  */
};

/** Where the record came from. */
enum stationCarrier {
    STATION_CARRIER_APRS = 0,
    STATION_CARRIER_M17,
};

/** What a record describes. */
enum stationKind {
    STATION_KIND_STATION = 0,
    STATION_KIND_OBJECT,  /**< Placed on the map by another station. */
    STATION_KIND_ITEM,
    STATION_KIND_WEATHER, /**< Sends weather reports.          */
};

struct station {
    char call[STATION_CALL_LEN];
    uint8_t valid;      /**< enum stationValid bits.   */
    uint8_t carrier;    /**< enum stationCarrier.      */
    int32_t latitude;   /**< Millionths of a degree, positive north. */
    int32_t longitude;  /**< Millionths of a degree, positive east.  */
    uint32_t lastHeard; /**< Seconds of system tick. */
    int16_t altitude;   /**< Metres.                 */
    uint16_t speed;     /**< km/h.                   */
    int16_t course;     /**< Degrees, 0-359.         */
    uint16_t hdop;      /**< Centimetres.            */
    uint8_t kind;       /**< enum stationKind.       */
    char symTable;
    char symCode;
    struct aprsFreq freq; /**< The last one it announced. */
};

/** Empty the table. */
void stations_init(void);

/**
 * The record for a callsign, created if there is none, moved to the front
 * and stamped with @p now. With the table full the oldest is dropped.
 * Fields other than call, carrier and lastHeard are left for the caller.
 *
 * @return the record, or NULL on a bad callsign.
 */
struct station *stations_update(const char *call, uint8_t carrier,
                                uint32_t now);

/** Forget a station, e.g. an object that was taken off the map. */
void stations_remove(const char *call);

/** Drop every record not heard for more than @p maxAge seconds. */
void stations_expire(uint32_t now, uint32_t maxAge);

/** The record for a callsign, or NULL. */
const struct station *stations_find(const char *call);

size_t stations_count(void);

/** Record by position, newest first; NULL if out of range. */
const struct station *stations_get(size_t idx);

/**
 * Great-circle distance and initial bearing from one point to another,
 * both in millionths of a degree.
 *
 * @param bearing: degrees from north, 0-359; may be NULL.
 * @return distance in metres.
 */
uint32_t stations_distance(int32_t lat1, int32_t lon1, int32_t lat2,
                           int32_t lon2, uint16_t *bearing);

#ifdef __cplusplus
}
#endif

#endif /* STATIONS_H */
