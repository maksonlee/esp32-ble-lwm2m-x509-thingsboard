#include "dht11.h"
#include "dht11_decode.h"
#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "sdkconfig.h"

/* Owned by the telemetry task. RMT captures edges even during interrupts. */
static rmt_channel_handle_t channel;
static QueueHandle_t received;
static rmt_symbol_word_t symbols[64];

static bool on_received(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *event, void *arg)
{
    BaseType_t wake = pdFALSE;
    size_t count = event->num_symbols;
    xQueueSendFromISR(received, &count, &wake);
    return wake == pdTRUE;
}

static esp_err_t init_receiver(void)
{
    received = xQueueCreate(1, sizeof(size_t));
    if (!received) { return ESP_ERR_NO_MEM; }
    rmt_rx_channel_config_t config = {
        .gpio_num = CONFIG_DHT11_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,
        .mem_block_symbols = 64,
    };
    esp_err_t err = rmt_new_rx_channel(&config, &channel);
    if (err == ESP_OK) {
        rmt_rx_event_callbacks_t callbacks = {.on_recv_done = on_received};
        err = rmt_rx_register_event_callbacks(channel, &callbacks, NULL);
    }
    if (err == ESP_OK) { err = gpio_set_direction(CONFIG_DHT11_GPIO, GPIO_MODE_INPUT_OUTPUT_OD); }
    if (err == ESP_OK) { err = gpio_set_pull_mode(CONFIG_DHT11_GPIO, GPIO_PULLUP_ONLY); }
    if (err == ESP_OK) { err = gpio_set_level(CONFIG_DHT11_GPIO, 1); }
    if (err != ESP_OK) {
        if (channel) { rmt_del_channel(channel); channel = NULL; }
        vQueueDelete(received); received = NULL;
    } else {
        /* The pin was floating/input before initialization. Give the sensor a
         * stable released bus before the first start edge (also on cold boot). */
        vTaskDelay(pdMS_TO_TICKS(1000) + 1);
    }
    return err;
}

esp_err_t dht11_read(dht11_reading_t *result)
{
    if (!result) { return ESP_ERR_INVALID_ARG; }
    esp_err_t err = channel ? ESP_OK : init_receiver();
    if (err != ESP_OK) { return err; }
    err = gpio_set_level(CONFIG_DHT11_GPIO, 0);
    if (err != ESP_OK) { return err; }
    /* An extra tick guarantees at least 20 ms even at the end of a tick. */
    vTaskDelay(pdMS_TO_TICKS(20) + 1);
    xQueueReset(received);
    err = rmt_enable(channel);
    if (err != ESP_OK) { gpio_set_level(CONFIG_DHT11_GPIO, 1); return err; }
    rmt_receive_config_t config = {
        .signal_range_min_ns = 1000,
        .signal_range_max_ns = 150000,
    };
    err = rmt_receive(channel, symbols, sizeof(symbols), &config);
    /* Open-drain HIGH releases the bus; never drive against the sensor. */
    esp_err_t release_err = gpio_set_level(CONFIG_DHT11_GPIO, 1);
    size_t count = 0;
    if (err == ESP_OK) { err = release_err; }
    if (err == ESP_OK && xQueueReceive(received, &count, pdMS_TO_TICKS(100)) != pdTRUE) {
        err = ESP_ERR_TIMEOUT;
    }
    esp_err_t stop_err = rmt_disable(channel);
    if (err == ESP_OK) { err = stop_err; }
    if (err != ESP_OK) { return err; }
    if (count > 64) { return ESP_ERR_INVALID_SIZE; }
    dht11_pulse_t pulses[128];
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        if (symbols[i].duration0) {
            pulses[used++] = (dht11_pulse_t){symbols[i].duration0, symbols[i].level0};
        }
        if (symbols[i].duration1) {
            pulses[used++] = (dht11_pulse_t){symbols[i].duration1, symbols[i].level1};
        }
    }
    return dht11_decode(pulses, used, result);
}
