#include "time_sync.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <stdatomic.h>

static bool started;
static atomic_bool synchronized;
static const char *TAG = "time_sync";

static void on_sync(struct timeval *tv)
{
    /* Refuse implausible dates; X.509 validation checks the actual validity span. */
    atomic_store(&synchronized, tv->tv_sec >= 1704067200LL && tv->tv_sec < 4102444800LL);
}

bool time_sync_wait(void)
{
    if (atomic_load(&synchronized)) { return true; }
    if (!started) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_TIME_SERVER);
        config.sync_cb = on_sync;
        esp_err_t err = esp_netif_sntp_init(&config);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SNTP initialization failed: %s", esp_err_to_name(err));
            return false;
        }
        started = true;
    }
    esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
    if (!atomic_load(&synchronized)) {
        ESP_LOGW(TAG, "Waiting for NTP; TLS startup remains blocked");
        return false;
    }
    ESP_LOGI(TAG, "Clock synchronized; certificate date verification enabled");
    return true;
}
