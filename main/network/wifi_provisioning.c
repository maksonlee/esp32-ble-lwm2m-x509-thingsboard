#include "wifi_provisioning.h"
#include "lwm2m/lwm2m_client_handler.h"
#include "storage/spiffs_utils.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "maintenance/firmware_update.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"

static const char *TAG = "wifi_prov";

static int retry_count = 0;
static char *provisioning_pop;

static void clear_provisioning_pop(void)
{
    if (provisioning_pop) {
        size_t n = strlen(provisioning_pop);
        volatile char *p = provisioning_pop;
        while (n--) { *p++ = 0; }
        free(provisioning_pop);
        provisioning_pop = NULL;
    }
}
static EventGroupHandle_t wifi_state;
static TaskHandle_t reconnect_task;
#define WIFI_RETRY_BIT BIT0
#define WIFI_PROVISIONING_BIT BIT1
#define WIFI_ASSOCIATED_BIT BIT2
#define WIFI_RESET_BIT BIT3

static void reconnect_worker(void *arg)
{
    for (;;) {
        xEventGroupWaitBits(wifi_state, WIFI_RETRY_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000))) {
            continue;
        }
        EventBits_t state = xEventGroupGetBits(wifi_state);
        if ((state & WIFI_RETRY_BIT) && !(state & (WIFI_PROVISIONING_BIT | WIFI_RESET_BIT))) {
            esp_err_t err = esp_wifi_connect();
            ESP_LOGI(TAG, "Wi-Fi reconnect attempt %d: %s", ++retry_count, esp_err_to_name(err));
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == NETWORK_PROV_EVENT && event_id == NETWORK_PROV_END) {
        esp_err_t err = network_prov_mgr_deinit();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Provisioning cleanup failed: %s", esp_err_to_name(err));
        }
        if (err == ESP_OK) { clear_provisioning_pop(); }
        xEventGroupClearBits(wifi_state, WIFI_PROVISIONING_BIT);
        if (!(xEventGroupGetBits(wifi_state) & WIFI_ASSOCIATED_BIT)) {
            xEventGroupSetBits(wifi_state, WIFI_RETRY_BIT);
        }
    }
    else if (event_base == NETWORK_PROV_EVENT && event_id == NETWORK_PROV_WIFI_CRED_FAIL) {
        ESP_LOGW(TAG, "Wi-Fi credentials rejected; allowing another BLE provisioning attempt");
        esp_err_t err = network_prov_mgr_reset_wifi_sm_state_on_failure();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot reset provisioning state: %s", esp_err_to_name(err));
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(TAG, "Wi-Fi STA Started");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    {
        xEventGroupClearBits(wifi_state, WIFI_RETRY_BIT);
        xEventGroupSetBits(wifi_state, WIFI_ASSOCIATED_BIT);
        xTaskNotifyGive(reconnect_task);
        ESP_LOGI(TAG, "Connected to Wi-Fi");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        retry_count = 0;
        ESP_LOGI(TAG, "Got IP address");
        lwm2m_app_set_network(true);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP)
    {
        lwm2m_app_set_network(false);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGW(TAG, "Wi-Fi disconnected");
        lwm2m_app_set_network(false);
        xEventGroupClearBits(wifi_state, WIFI_ASSOCIATED_BIT);

        /* The provisioning manager owns connection attempts during enrollment. */
        if (!(xEventGroupGetBits(wifi_state) & (WIFI_PROVISIONING_BIT | WIFI_RESET_BIT))) {
            xEventGroupSetBits(wifi_state, WIFI_RETRY_BIT);
            xTaskNotifyGive(reconnect_task);
        }
    }
}

void wifi_provisioning_start(void)
{
    wifi_state = xEventGroupCreate();
    ESP_ERROR_CHECK(wifi_state ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(reconnect_worker, "wifi_retry", 3072, NULL, 4,
                               &reconnect_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    // Initialize NVS with fallback logic
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        /* A candidate must not destroy Wi-Fi credentials or the OTA journal
         * when an NVS migration fails. Reset lets the bootloader roll back. */
        if (firmware_update_boot_pending()) {
            ESP_ERROR_CHECK(ret);
        }
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    else
    {
        ESP_ERROR_CHECK(ret);
    }

    // Initialize network interfaces
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();

    // Register Wi-Fi and IP event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));

    // Initialize Wi-Fi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Check if device is already provisioned
    bool provisioned = false;
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));

    if (!provisioned)
    {
        xEventGroupSetBits(wifi_state, WIFI_PROVISIONING_BIT);
        ESP_LOGI(TAG, "Starting Wi-Fi provisioning via BLE");

        if (spiffs_mount()) {
            provisioning_pop = spiffs_read_file("/spiffs/provisioning.pop");
        }
        const char *allowed = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-";
        if (!provisioning_pop || strlen(provisioning_pop) < 16 || strlen(provisioning_pop) > 128 ||
            strspn(provisioning_pop, allowed) != strlen(provisioning_pop)) {
            ESP_LOGE(TAG, "Missing or invalid per-device PoP; install it with the USB maintenance tool");
            clear_provisioning_pop();
            return;
        }

        // Generate BLE device name using MAC
        char service_name[13] = {0}; // PROV_ + 6 hex digits + null terminator
        uint8_t mac[6];
        ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
        snprintf(service_name, sizeof(service_name), "PROV_%02X%02X%02X", mac[3], mac[4], mac[5]);
        ESP_LOGI(TAG, "BLE Device Name: %s", service_name);

        // Set up BLE provisioning configuration
        network_prov_mgr_config_t config = {
            .scheme = network_prov_scheme_ble,
            .network_prov_wifi_conn_cfg.wifi_conn_attempts = 3,
            .scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM};

        ESP_ERROR_CHECK(network_prov_mgr_init(config));
        ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1, provisioning_pop, service_name, NULL));
    }
    else
    {
        ESP_LOGI(TAG, "Already provisioned, connecting to saved Wi-Fi");
        xEventGroupSetBits(wifi_state, WIFI_RETRY_BIT);
    }
}

void wifi_provisioning_reset(void)
{
    xEventGroupSetBits(wifi_state, WIFI_RESET_BIT);
    xEventGroupClearBits(wifi_state, WIFI_RETRY_BIT);
    lwm2m_app_set_network(false);
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_restore();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot clear Wi-Fi settings: %s", esp_err_to_name(err));
        xEventGroupClearBits(wifi_state, WIFI_RESET_BIT);
        xEventGroupSetBits(wifi_state, WIFI_RETRY_BIT);
        return;
    }
    esp_restart();
}
