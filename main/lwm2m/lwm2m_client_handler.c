#include "lwm2m_client_handler.h"
#include "lwm2m_sensor.h"
#include "lwm2m_firmware.h"
#include "storage/cert_manager.h"
#include "network/time_sync.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"
#include <stdatomic.h>
#include <string.h>
#include <anjay/security.h>
#include <anjay/server.h>
#include <avsystem/commons/avs_crypto_pki.h>
#include <avsystem/commons/avs_log.h>

static const char *TAG = "lwm2m_client";
#define NETWORK_READY_BIT BIT0
#define SERVER_SSID 123
#define RETRY_MS 5000
#define SAMPLE_INTERVAL_US ((int64_t)CONFIG_TELEMETRY_INTERVAL_SECONDS * 1000000)

static EventGroupHandle_t network_state;
static TaskHandle_t worker_handle;
static atomic_uint network_generation;
static anjay_t *client;
static lwm2m_sensor_t sensors[2];

static bool network_ready(void)
{
    return (xEventGroupGetBits(network_state) & NETWORK_READY_BIT) != 0;
}

static bool registered(void)
{
    anjay_registration_expiration_status_t status;
    avs_time_real_t expiration = anjay_registration_expiration_time_with_status(
            client, SERVER_SSID, &status);
    return avs_time_real_valid(expiration)
            && status == ANJAY_REGISTRATION_EXPIRATION_STATUS_VALID
            && !anjay_ongoing_registration_exists(client)
            && anjay_get_socket_entries(client) != NULL;
}

static void notification_status(anjay_t *anjay, anjay_ssid_t ssid,
                                 const anjay_uri_path_t *paths, size_t count,
                                 avs_error_t error)
{
    if (avs_is_ok(error)) {
        ESP_LOGI(TAG, "Server acknowledged sensor notification");
    } else {
        ESP_LOGW(TAG, "Sensor notification failed: category=%u code=%u",
                 (unsigned)error.category, (unsigned)error.code);
    }
}

static void stop_client(void)
{
    if (client) {
        /* Drop sockets and pending notifications before a new network session. */
        anjay_transport_enter_offline(client, ANJAY_TRANSPORT_SET_ALL);
        anjay_sched_run(client);
        anjay_delete(client);
        client = NULL;
    }
    lwm2m_firmware_disconnect();
    lwm2m_sensors_invalidate(sensors);
    cert_manager_free();
}

static bool start_client(void)
{
    if (strncmp(CONFIG_LWM2M_SERVER_URI, "coaps://", 8)
            || !time_sync_wait() || !network_ready() || !cert_manager_load()) {
        return false;
    }
    const anjay_lwm2m_version_config_t version = {
        /* ThingsBoard requires the unquoted object versions used by 1.1. */
        .minimum_version = ANJAY_LWM2M_VERSION_1_1,
        .maximum_version = ANJAY_LWM2M_VERSION_1_1
    };
    static uint32_t ciphers[] = { 0xc0ae, 0xc023 };
    const anjay_configuration_t config = {
        .endpoint_name = CONFIG_LWM2M_ENDPOINT,
        .in_buffer_size = 8192,
        .out_buffer_size = 8192,
        .lwm2m_version_config = &version,
        .trust_store_certs = avs_crypto_certificate_chain_info_from_buffer(
                cert_ca, strlen(cert_ca)),
        .default_tls_ciphersuites = { .ids = ciphers, .num_ids = 2 },
        .confirmable_notifications = true,
        .stored_notification_limit = 2,
        .confirmable_notification_status_cb = notification_status
    };
    client = anjay_new(&config);
    if (!client) {
        stop_client();
        return false;
    }
    const uint8_t pkix = 0;
    const anjay_security_instance_t security = {
        .ssid = SERVER_SSID,
        .server_uri = CONFIG_LWM2M_SERVER_URI,
        .security_mode = ANJAY_SECURITY_CERTIFICATE,
        .public_cert = avs_crypto_certificate_chain_info_from_buffer(
                cert_client, strlen(cert_client)),
        .private_key = avs_crypto_private_key_info_from_buffer(
                key_client, strlen(key_client), NULL),
        .certificate_usage = &pkix
    };
    const anjay_server_instance_t server = {
        .ssid = SERVER_SSID,
        .lifetime = 300,
        .default_min_period = 0,
        .default_max_period = -1,
        .disable_timeout = -1,
        .notification_storing = false,
        .binding = "U"
    };
    anjay_iid_t security_iid = ANJAY_ID_INVALID;
    anjay_iid_t server_iid = ANJAY_ID_INVALID;
    if (anjay_security_object_install(client)
            || anjay_server_object_install(client)
            || anjay_security_object_add_instance(client, &security, &security_iid)
            || anjay_server_object_add_instance(client, &server, &server_iid)
            || lwm2m_sensors_install(client, sensors)
            || lwm2m_firmware_install(client)) {
        stop_client();
        return false;
    }
    return true;
}

static void sample_sensor(unsigned generation)
{
    dht11_reading_t reading;
    esp_err_t error = dht11_read(&reading);
    if (error != ESP_OK) {
        lwm2m_sensors_invalidate(sensors);
        ESP_LOGW(TAG, "DHT11 read failed: %s", esp_err_to_name(error));
        return;
    }
    /* Also reject a disconnect/reconnect that completed during the read. */
    if (!network_ready() || generation != atomic_load(&network_generation)
            || !registered()) {
        lwm2m_sensors_invalidate(sensors);
        return;
    }
    if (lwm2m_sensors_update(client, sensors, &reading, time(NULL))) {
        ESP_LOGW(TAG, "Could not schedule sensor notifications");
    } else {
        ESP_LOGI(TAG, "Sampled temperature=%d humidity=%d",
                 reading.temperature, reading.humidity);
    }
}

static void lwm2m_worker(void *argument)
{
    avs_log_set_default_level(AVS_LOG_INFO);
    while (true) {
        xEventGroupWaitBits(network_state, NETWORK_READY_BIT,
                            pdFALSE, pdTRUE, portMAX_DELAY);
        unsigned generation = atomic_load(&network_generation);
        if (!start_client()) {
            ESP_LOGW(TAG, "LwM2M startup failed; retrying in 5 seconds");
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RETRY_MS));
            continue;
        }
        bool connected = false;
        int64_t next_sample = 0;
        while (network_ready() && generation == atomic_load(&network_generation)) {
            anjay_sched_run(client);
            if (!network_ready() || generation != atomic_load(&network_generation)) {
                break;
            }
            bool ready = registered();
            if (ready) {
                lwm2m_firmware_registered(client);
            }
            int64_t now = esp_timer_get_time();
            if (ready != connected) {
                connected = ready;
                next_sample = now + SAMPLE_INTERVAL_US;
                lwm2m_sensors_invalidate(sensors);
                ESP_LOGI(TAG, "LwM2M registration %s", ready ? "ready" : "unavailable");
            }
            if (ready && now >= next_sample) {
                sample_sensor(generation);
                /* Wait a full sample interval after the read completes,
                 * including the first DHT11 bus-settling delay. */
                next_sample = esp_timer_get_time() + SAMPLE_INTERVAL_US;
            }
            if (anjay_all_connections_failed(client)) {
                break;
            }
            /* All Anjay calls and sensor access belong to this task. */
            anjay_serve_any(client, avs_time_duration_from_scalar(100, AVS_TIME_MS));
        }
        stop_client();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RETRY_MS));
    }
}

void lwm2m_app_start(void)
{
    if (!network_state) {
        network_state = xEventGroupCreate();
        ESP_ERROR_CHECK(network_state ? ESP_OK : ESP_ERR_NO_MEM);
    }
    if (!worker_handle && xTaskCreate(lwm2m_worker, "lwm2m", 16384, NULL, 4,
                                     &worker_handle) != pdPASS) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
}

void lwm2m_app_set_network(bool available)
{
    if (available) {
        xEventGroupSetBits(network_state, NETWORK_READY_BIT);
    } else {
        xEventGroupClearBits(network_state, NETWORK_READY_BIT);
    }
    atomic_fetch_add(&network_generation, 1);
    xTaskNotifyGive(worker_handle);
}
