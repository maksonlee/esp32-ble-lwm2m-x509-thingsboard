/* Fault injection at the transport boundary, using real Anjay public types. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <anjay/anjay.h>
#include <anjay/security.h>
#include <anjay/server.h>
#include <avsystem/commons/avs_crypto_pki.h>
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "../../main/storage/cert_manager.h"
#include "../../main/network/time_sync.h"
#include "../../main/sensors/dht11.h"
#include "../../main/lwm2m/lwm2m_client_handler.h"
#include "../../main/lwm2m/lwm2m_firmware.h"

static unsigned bits, tasks, loads, frees, creates, destroys, notifications;
static unsigned firmware_installs, firmware_confirmations, firmware_disconnects;
static int fail_new, fail_install, fail_add, fail_register, fail_firmware, read_error;
static bool synced = true, load_ok = true, registration_ok = true;
static bool expiration_valid = true, registration_ongoing, socket_available = true;
static bool disconnect_in_read, reconnect_in_read;
static double returned_value;
static int64_t returned_timestamp;
static int fake_client;
static anjay_socket_entry_t fake_socket;
static bool run_worker;
static unsigned worker_waits, worker_reads, retry_waits;
static int64_t clock_us, worker_connected_at, first_confirmation = -1;
static int64_t read_starts[2], read_ends[2];
static jmp_buf worker_finished;
char *cert_ca = "test CA";
char *cert_client = "test device";
char *key_client = "test key placeholder";

int lwm2m_firmware_install(anjay_t *anjay) {
    assert(anjay);
    ++firmware_installs;
    if (fail_firmware) {
        if (run_worker) --fail_firmware;
        return -1;
    }
    if (run_worker) worker_connected_at = clock_us;
    return 0;
}
void lwm2m_firmware_registered(anjay_t *anjay) {
    assert(anjay && bits && registration_ok && expiration_valid
            && !registration_ongoing && socket_available);
    ++firmware_confirmations;
    if (first_confirmation < 0) first_confirmation = clock_us;
}
void lwm2m_firmware_disconnect(void) { ++firmware_disconnects; }

bool time_sync_wait(void) { return synced; }
bool cert_manager_load(void) { ++loads; return load_ok; }
void cert_manager_free(void) { ++frees; }
int64_t esp_timer_get_time(void) { return clock_us; }
EventGroupHandle_t xEventGroupCreate(void) { return &bits; }
EventBits_t xEventGroupGetBits(EventGroupHandle_t group) { return *group; }
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t value) { return *group |= value; }
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t value) { return *group &= ~value; }
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t value, BaseType_t clear,
                               BaseType_t all, TickType_t timeout) {
    if (run_worker && worker_waits++ >= 2) longjmp(worker_finished, 1);
    return *group;
}
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg,
                      unsigned priority, TaskHandle_t *handle) {
    ++tasks; *handle = (void *)1; return pdPASS;
}
unsigned ulTaskNotifyTake(BaseType_t clear, TickType_t timeout) {
    if (run_worker) {
        assert(timeout == pdMS_TO_TICKS(5000));
        ++retry_waits;
        clock_us += 5000000;
    }
    return 0;
}
void xTaskNotifyGive(TaskHandle_t task) { assert(task); }
esp_err_t dht11_read(dht11_reading_t *result) {
    if (run_worker) {
        assert(worker_reads < 2);
        read_starts[worker_reads] = clock_us;
        /* The first read includes one second of DHT11 bus settling. */
        clock_us += worker_reads ? 50000 : 1050000;
        read_ends[worker_reads++] = clock_us;
    }
    if (disconnect_in_read) {
        lwm2m_app_set_network(false);
        if (reconnect_in_read) lwm2m_app_set_network(true);
    }
    *result = (dht11_reading_t){23, 49};
    return read_error ? ESP_ERR_INVALID_CRC : ESP_OK;
}

static anjay_t *fake_new(const anjay_configuration_t *config) {
    ++creates;
    assert(config->confirmable_notifications && config->stored_notification_limit == 2);
    assert(config->lwm2m_version_config->minimum_version == ANJAY_LWM2M_VERSION_1_1);
    assert(config->lwm2m_version_config->maximum_version == ANJAY_LWM2M_VERSION_1_1);
    assert(config->default_tls_ciphersuites.num_ids == 2);
    return fail_new ? NULL : (anjay_t *)&fake_client;
}
static void fake_delete(anjay_t *anjay) { ++destroys; }
static int fake_offline(anjay_t *anjay, anjay_transport_set_t set) { return 0; }
static void fake_sched(anjay_t *anjay) {}
static bool fake_connections_failed(anjay_t *anjay) { return false; }
static int fake_serve(anjay_t *anjay, avs_time_duration_t timeout) {
    assert(run_worker && clock_us < 30000000);
    clock_us += 100000;
    /* Independently exercise every registration gate before confirmation. */
    int64_t connected_for = clock_us - worker_connected_at;
    registration_ok = connected_for >= 1000000;
    expiration_valid = connected_for >= 2000000;
    registration_ongoing = connected_for < 3000000;
    socket_available = connected_for >= 4000000;
    if (worker_reads == 2) lwm2m_app_set_network(false);
    return 0;
}
static int fake_install(anjay_t *anjay) { return fail_install; }
static int fake_security_add(anjay_t *anjay, const anjay_security_instance_t *instance, anjay_iid_t *iid) {
    assert(instance->security_mode == ANJAY_SECURITY_CERTIFICATE);
    assert(instance->certificate_usage && *instance->certificate_usage == 0);
    return fail_add;
}
static int fake_server_add(anjay_t *anjay, const anjay_server_instance_t *instance, anjay_iid_t *iid) {
    assert(!instance->notification_storing && instance->default_max_period == -1);
    return fail_add;
}
static int fake_register_object(anjay_t *anjay, const anjay_dm_object_def_t *const *object) {
    assert((*object)->oid == 3303 || (*object)->oid == 3304);
    assert(strcmp((*object)->version, "1.1") == 0);
    return fail_register;
}
static avs_time_real_t fake_expiration(anjay_t *anjay, anjay_ssid_t ssid,
                                      anjay_registration_expiration_status_t *status) {
    *status = registration_ok ? ANJAY_REGISTRATION_EXPIRATION_STATUS_VALID
                              : ANJAY_REGISTRATION_EXPIRATION_STATUS_EXPIRED;
    return expiration_valid ? avs_time_real_from_scalar(1900000000, AVS_TIME_S)
                            : AVS_TIME_REAL_INVALID;
}
static bool fake_ongoing(anjay_t *anjay) { return registration_ongoing; }
static const anjay_socket_entry_t *fake_sockets(anjay_t *anjay) {
    return socket_available ? &fake_socket : NULL;
}
static int fake_notify(anjay_t *anjay, anjay_oid_t oid, anjay_iid_t iid, anjay_rid_t rid) {
    assert((oid == 3303 || oid == 3304) && iid == 0 && (rid == 5700 || rid == 5518));
    ++notifications; return 0;
}
static int fake_ret_double(anjay_output_ctx_t *ctx, double value) { returned_value = value; return 0; }
static int fake_ret_i64(anjay_output_ctx_t *ctx, int64_t value) { returned_timestamp = value; return 0; }

#define anjay_new fake_new
#define anjay_delete fake_delete
#define anjay_transport_enter_offline fake_offline
#define anjay_sched_run fake_sched
#define anjay_all_connections_failed fake_connections_failed
#define anjay_serve_any fake_serve
#define anjay_security_object_install fake_install
#define anjay_server_object_install fake_install
#define anjay_security_object_add_instance fake_security_add
#define anjay_server_object_add_instance fake_server_add
#define anjay_register_object fake_register_object
#define anjay_registration_expiration_time_with_status fake_expiration
#define anjay_ongoing_registration_exists fake_ongoing
#define anjay_get_socket_entries fake_sockets
#define anjay_notify_changed fake_notify
#define anjay_ret_double fake_ret_double
#define anjay_ret_i64 fake_ret_i64
#include "../../main/lwm2m/lwm2m_sensor.c"
#include "../../main/lwm2m/lwm2m_client_handler.c"

int main(void) {
    lwm2m_app_start(); lwm2m_app_start(); assert(tasks == 1);
    lwm2m_app_set_network(true);
    synced = false; assert(!start_client()); assert(!creates && !loads);
    synced = true; load_ok = false; assert(!start_client()); assert(!creates);
    load_ok = true; fail_new = 1; assert(!start_client()); assert(!client && frees == 1);
    fail_new = 0; fail_install = 1; assert(!start_client()); assert(!client && destroys == 1);
    fail_install = 0; fail_add = 1; assert(!start_client()); assert(!client && destroys == 2);
    fail_add = 0; fail_register = 1; assert(!start_client()); assert(!client && destroys == 3);
    fail_register = 0; fail_firmware = 1;
    assert(!start_client()); assert(!client && destroys == 4 && frees == 5);
    assert(firmware_installs == 1 && firmware_disconnects == 5);
    fail_firmware = 0; assert(start_client()); assert(client);
    assert(firmware_installs == 2 && firmware_confirmations == 0);

    assert(resource_read(client, &sensors[0].def, 0, 5700, ANJAY_ID_INVALID, NULL) == ANJAY_ERR_NOT_FOUND);
    sample_sensor(atomic_load(&network_generation)); assert(notifications == 4);
    assert(resource_read(client, &sensors[0].def, 0, 5700, ANJAY_ID_INVALID, NULL) == 0);
    assert(returned_value == 23);
    dht11_reading_t identical = {23, 49};
    lwm2m_sensors_update(client, sensors, &identical, 1900000005);
    assert(notifications == 8);
    assert(resource_read(client, &sensors[0].def, 0, 5518, ANJAY_ID_INVALID, NULL) == 0);
    assert(returned_timestamp == 1900000005);
    assert(resource_read(client, &sensors[1].def, 0, 5700, ANJAY_ID_INVALID, NULL) == 0);
    assert(returned_value == 49);

    read_error = 1; sample_sensor(atomic_load(&network_generation));
    assert(notifications == 8 && !sensors[0].valid && !sensors[1].valid);
    assert(resource_read(client, &sensors[0].def, 0, 5700, ANJAY_ID_INVALID, NULL) == ANJAY_ERR_NOT_FOUND);
    read_error = 0; disconnect_in_read = true;
    sample_sensor(atomic_load(&network_generation)); assert(notifications == 8);
    lwm2m_app_set_network(true); reconnect_in_read = true;
    sample_sensor(atomic_load(&network_generation)); assert(notifications == 8);
    disconnect_in_read = false; registration_ok = false;
    sample_sensor(atomic_load(&network_generation)); assert(notifications == 8);
    registration_ok = true; sample_sensor(atomic_load(&network_generation)); assert(notifications == 12);
    stop_client(); assert(!client && !sensors[0].valid);
    unsigned destroyed_before = destroys, freed_before = frees;
    unsigned disconnected_before = firmware_disconnects;
    fail_firmware = 1;
    registration_ok = expiration_valid = socket_available = false;
    registration_ongoing = true;
    run_worker = true;
    lwm2m_app_set_network(true);
    if (!setjmp(worker_finished)) lwm2m_worker(NULL);
    assert(worker_reads == 2 && !client);
    assert(retry_waits == 2 && firmware_installs == 4);
    assert(destroys == destroyed_before + 2 && frees == freed_before + 2);
    assert(firmware_disconnects == disconnected_before + 2);
    assert(firmware_confirmations > 0 && first_confirmation == worker_connected_at + 4000000);
    assert(read_starts[0] >= first_confirmation + 5000000);
    assert(read_starts[1] - read_ends[0] >= 5000000);
    puts("LwM2M startup/retry, OTA confirmation gates, sample freshness and disconnect races: PASS");
    return 0;
}
