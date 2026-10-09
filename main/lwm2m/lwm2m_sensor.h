#pragma once
#include <stdbool.h>
#include <time.h>
#include <anjay/anjay.h>
#include "sensors/dht11.h"

/* The LwM2M task owns these objects and all accesses to their sample. */
typedef struct {
    const anjay_dm_object_def_t *def;
    int value;
    time_t timestamp;
    bool valid;
} lwm2m_sensor_t;

int lwm2m_sensors_install(anjay_t *anjay, lwm2m_sensor_t sensors[2]);
void lwm2m_sensors_invalidate(lwm2m_sensor_t sensors[2]);
int lwm2m_sensors_update(anjay_t *anjay, lwm2m_sensor_t sensors[2],
                        const dht11_reading_t *reading, time_t timestamp);
