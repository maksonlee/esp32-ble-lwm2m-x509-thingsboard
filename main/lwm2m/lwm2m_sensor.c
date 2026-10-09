#include "lwm2m_sensor.h"

static lwm2m_sensor_t *sensor_from(const anjay_dm_object_def_t *const *object)
{
    return (lwm2m_sensor_t *)object;
}

static int list_instances(anjay_t *anjay, const anjay_dm_object_def_t *const *object,
                          anjay_dm_list_ctx_t *ctx)
{
    anjay_dm_emit(ctx, 0);
    return 0;
}

static int list_resources(anjay_t *anjay, const anjay_dm_object_def_t *const *object,
                          anjay_iid_t iid, anjay_dm_resource_list_ctx_t *ctx)
{
    if (iid != 0) {
        return ANJAY_ERR_NOT_FOUND;
    }
    anjay_dm_resource_presence_t present = sensor_from(object)->valid
            ? ANJAY_DM_RES_PRESENT : ANJAY_DM_RES_ABSENT;
    anjay_dm_emit_res(ctx, 5518, ANJAY_DM_RES_R, present);
    anjay_dm_emit_res(ctx, 5700, ANJAY_DM_RES_R, present);
    anjay_dm_emit_res(ctx, 5701, ANJAY_DM_RES_R, ANJAY_DM_RES_PRESENT);
    return 0;
}

static int resource_read(anjay_t *anjay, const anjay_dm_object_def_t *const *object,
                         anjay_iid_t iid, anjay_rid_t rid, anjay_riid_t riid,
                         anjay_output_ctx_t *ctx)
{
    lwm2m_sensor_t *sensor = sensor_from(object);
    if (iid != 0 || riid != ANJAY_ID_INVALID) {
        return ANJAY_ERR_NOT_FOUND;
    }
    if (rid == 5701) {
        return anjay_ret_string(ctx, sensor->def->oid == 3303 ? "Cel" : "%RH");
    }
    if (!sensor->valid) {
        return ANJAY_ERR_NOT_FOUND;
    }
    switch (rid) {
    case 5518:
        return anjay_ret_i64(ctx, sensor->timestamp);
    case 5700:
        /* IPSO Sensor Value is Float; the DHT11 reading remains integral. */
        return anjay_ret_double(ctx, sensor->value);
    default:
        return ANJAY_ERR_NOT_FOUND;
    }
}

/* Object version 1.1 includes the standard measurement timestamp resource. */
static const anjay_dm_object_def_t TEMPERATURE = {
    .oid = 3303, .version = "1.1",
    .handlers = {
        .list_instances = list_instances,
        .list_resources = list_resources,
        .resource_read = resource_read
    }
};
static const anjay_dm_object_def_t HUMIDITY = {
    .oid = 3304, .version = "1.1",
    .handlers = {
        .list_instances = list_instances,
        .list_resources = list_resources,
        .resource_read = resource_read
    }
};

int lwm2m_sensors_install(anjay_t *anjay, lwm2m_sensor_t sensors[2])
{
    sensors[0] = (lwm2m_sensor_t){ .def = &TEMPERATURE };
    sensors[1] = (lwm2m_sensor_t){ .def = &HUMIDITY };
    return anjay_register_object(anjay, &sensors[0].def)
            || anjay_register_object(anjay, &sensors[1].def);
}

void lwm2m_sensors_invalidate(lwm2m_sensor_t sensors[2])
{
    sensors[0].valid = sensors[1].valid = false;
}

int lwm2m_sensors_update(anjay_t *anjay, lwm2m_sensor_t sensors[2],
                        const dht11_reading_t *reading, time_t timestamp)
{
    sensors[0].value = reading->temperature;
    sensors[1].value = reading->humidity;
    int result = 0;
    for (unsigned i = 0; i < 2; ++i) {
        sensors[i].timestamp = timestamp;
        sensors[i].valid = true;
        /* Observe the instance: the timestamp changes even for equal readings.
         * No pmax timer is used, so a failed read cannot publish a cached value. */
        result |= anjay_notify_changed(anjay, sensors[i].def->oid, 0, 5700);
        result |= anjay_notify_changed(anjay, sensors[i].def->oid, 0, 5518);
    }
    return result;
}
