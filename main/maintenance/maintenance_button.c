#include "maintenance_button.h"
#include "button_hold.h"
#include "network/wifi_provisioning.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static void button_task(void *arg)
{
    button_hold_t hold = {0};
    for (;;) {
        if (button_hold_update(&hold, gpio_get_level(CONFIG_REPROVISION_GPIO) == 0, esp_timer_get_time())) {
            ESP_LOGW("maintenance", "Physical request: clear Wi-Fi settings and restart for BLE provisioning");
            wifi_provisioning_reset();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void maintenance_button_start(void)
{
    ESP_ERROR_CHECK(CONFIG_REPROVISION_GPIO == CONFIG_DHT11_GPIO ? ESP_ERR_INVALID_ARG : ESP_OK);
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << CONFIG_REPROVISION_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    ESP_ERROR_CHECK(xTaskCreate(button_task, "maintenance", 3072, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
