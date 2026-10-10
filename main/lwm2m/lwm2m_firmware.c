#include "lwm2m_firmware.h"
#include "maintenance/firmware_update.h"
#include <anjay/fw_update.h>

static bool reporting_result;
static bool failed_write;
static bool stream_active;
static bool single_zero_byte;
static size_t stream_bytes;

static int map_error(int error)
{
    switch (error) {
    case FIRMWARE_UPDATE_OK: return 0;
    case FIRMWARE_UPDATE_NO_SPACE: return ANJAY_FW_UPDATE_ERR_NOT_ENOUGH_SPACE;
    case FIRMWARE_UPDATE_NO_MEMORY: return ANJAY_FW_UPDATE_ERR_OUT_OF_MEMORY;
    case FIRMWARE_UPDATE_INTEGRITY: return ANJAY_FW_UPDATE_ERR_INTEGRITY_FAILURE;
    case FIRMWARE_UPDATE_UNSUPPORTED: return ANJAY_FW_UPDATE_ERR_UNSUPPORTED_PACKAGE_TYPE;
    default: return -1;
    }
}

static int stream_open(void *user, const char *uri, const struct anjay_etag *etag)
{
    /* The SDK is built without a downloader: only the authenticated server's
     * Package Write can supply bytes. Never fetch a Package URI. */
    failed_write = false;
    stream_bytes = 0;
    single_zero_byte = false;
    int result = uri ? -1 : map_error(firmware_update_begin());
    stream_active = result == 0;
    return result;
}
static int stream_write(void *user, const void *data, size_t size)
{
    int result = map_error(firmware_update_write(data, size));
    failed_write = result != 0;
    single_zero_byte = stream_bytes == 0 && size == 1
            && *(const unsigned char *)data == 0;
    stream_bytes += size;
    if (failed_write) {
        stream_active = false;
    }
    return result;
}
static int stream_finish(void *user)
{
    stream_active = false;
    return map_error(firmware_update_finish());
}
static void reset(void *user)
{
    /* set_result() and failed stream writes synchronously call reset too.
     * Those paths already cleaned up and must retain the durable outcome. */
    if (!reporting_result && !failed_write) {
        if (stream_active && !single_zero_byte) {
            /* Missing Block1 bytes can fail inside Anjay's input reader,
             * without calling our stream_write error path. */
            firmware_update_disconnect();
        } else {
            firmware_update_reset();
        }
    }
    failed_write = false;
    stream_active = false;
}
static int perform_upgrade(void *user) { return map_error(firmware_update_perform()); }
static const char *package_name(void *user) { return firmware_update_package_name(); }
static const char *package_version(void *user) { return firmware_update_package_version(); }

static const anjay_fw_update_handlers_t HANDLERS = {
    .stream_open = stream_open, .stream_write = stream_write,
    .stream_finish = stream_finish, .reset = reset,
    .perform_upgrade = perform_upgrade,
    .get_name = package_name, .get_version = package_version
};

static int list_instances(anjay_t *anjay, const anjay_dm_object_def_t *const *obj,
                          anjay_dm_list_ctx_t *ctx)
{
    anjay_dm_emit(ctx, 0);
    return 0;
}

static int list_resources(anjay_t *anjay, const anjay_dm_object_def_t *const *obj,
                          anjay_iid_t iid, anjay_dm_resource_list_ctx_t *ctx)
{
    anjay_dm_emit_res(ctx, 3, ANJAY_DM_RES_R, ANJAY_DM_RES_PRESENT);
    anjay_dm_emit_res(ctx, 4, ANJAY_DM_RES_E, ANJAY_DM_RES_PRESENT);
    anjay_dm_emit_res(ctx, 11, ANJAY_DM_RES_RM, ANJAY_DM_RES_PRESENT);
    anjay_dm_emit_res(ctx, 16, ANJAY_DM_RES_R, ANJAY_DM_RES_PRESENT);
    return 0;
}

static int list_resource_instances(anjay_t *anjay,
                                   const anjay_dm_object_def_t *const *obj,
                                   anjay_iid_t iid, anjay_rid_t rid,
                                   anjay_dm_list_ctx_t *ctx)
{
    if (rid != 11) {
        return ANJAY_ERR_NOT_FOUND;
    }
    anjay_dm_emit(ctx, 0);
    return 0;
}

static int resource_read(anjay_t *anjay, const anjay_dm_object_def_t *const *obj,
                         anjay_iid_t iid, anjay_rid_t rid, anjay_riid_t riid,
                         anjay_output_ctx_t *ctx)
{
    switch (rid) {
    case 3: return anjay_ret_string(ctx, firmware_update_running_version());
    case 11: return riid == 0 ? anjay_ret_i64(ctx, 0) : ANJAY_ERR_NOT_FOUND;
    case 16: return anjay_ret_string(ctx, "U");
    default: return ANJAY_ERR_NOT_FOUND;
    }
}

static int resource_execute(anjay_t *anjay, const anjay_dm_object_def_t *const *obj,
                            anjay_iid_t iid, anjay_rid_t rid,
                            anjay_execute_ctx_t *ctx)
{
    if (rid != 4) {
        return ANJAY_ERR_METHOD_NOT_ALLOWED;
    }
    firmware_update_request_reboot();
    return 0;
}

/* Running version plus the mandatory resources of the standard Device object.
 * No factory reset, credential management or writable clock is exposed. */
static const anjay_dm_object_def_t DEVICE = {
    .oid = 3,
    .handlers = {
        .list_instances = list_instances,
        .list_resources = list_resources,
        .list_resource_instances = list_resource_instances,
        .resource_read = resource_read,
        .resource_execute = resource_execute
    }
};
static const anjay_dm_object_def_t *device = &DEVICE;

int lwm2m_firmware_install(anjay_t *anjay)
{
    failed_write = false;
    stream_active = false;
    if (firmware_update_init() != ESP_OK) {
        return -1;
    }
    anjay_fw_update_initial_state_t initial = {0};
    switch (firmware_update_status()) {
    case FIRMWARE_UPDATE_DOWNLOADED: initial.result = ANJAY_FW_UPDATE_INITIAL_DOWNLOADED; break;
    case FIRMWARE_UPDATE_UPDATING: initial.result = ANJAY_FW_UPDATE_INITIAL_UPDATING; break;
    case FIRMWARE_UPDATE_SUCCESS: initial.result = ANJAY_FW_UPDATE_INITIAL_SUCCESS; break;
    case FIRMWARE_UPDATE_FAILURE: initial.result = ANJAY_FW_UPDATE_INITIAL_FAILED; break;
    default: initial.result = ANJAY_FW_UPDATE_INITIAL_NEUTRAL; break;
    }
    return anjay_register_object(anjay, &device)
            || anjay_fw_update_install(anjay, &HANDLERS, NULL, &initial);
}

void lwm2m_firmware_registered(anjay_t *anjay)
{
    bool updating = firmware_update_status() == FIRMWARE_UPDATE_UPDATING;
    if (firmware_update_confirm() == ESP_OK && updating) {
        reporting_result = true;
        anjay_fw_update_set_result(anjay, ANJAY_FW_UPDATE_RESULT_SUCCESS);
        reporting_result = false;
    }
}

void lwm2m_firmware_disconnect(void)
{
    firmware_update_disconnect();
    stream_active = failed_write = false;
}
