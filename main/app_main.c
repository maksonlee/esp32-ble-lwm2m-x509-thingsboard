#include "network/wifi_provisioning.h"
#include "lwm2m/lwm2m_client_handler.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_ota_ops.h"
#include "maintenance/maintenance_button.h"
#include "maintenance/firmware_update.h"

static const char *TAG = "app_main";

void app_main(void)
{
    ESP_ERROR_CHECK(firmware_update_boot_guard_start());

    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "System init: %s %s, running partition %s",
             app->project_name, app->version,
             running ? running->label : "unknown");

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    lwm2m_app_start();
    wifi_provisioning_start();
    maintenance_button_start();
    firmware_update_local_ready();
}
