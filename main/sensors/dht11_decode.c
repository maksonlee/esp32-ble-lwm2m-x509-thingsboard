#include "dht11_decode.h"

static bool within(uint16_t value, unsigned min, unsigned max)
{
    return value >= min && value <= max;
}

esp_err_t dht11_decode(const dht11_pulse_t *pulses, size_t count, dht11_reading_t *out)
{
    if (!pulses || !out) { return ESP_ERR_INVALID_ARG; }
    size_t start = 0;
    /* Find the sensor's 80 us low/high response after the host release. */
    while (start + 1 < count) {
        if (pulses[start].level == 0 && within(pulses[start].us, 65, 100) &&
            pulses[start + 1].level == 1 && within(pulses[start + 1].us, 65, 100)) {
            break;
        }
        start++;
    }
    start += 2;
    if (start > count || count - start < 80) { return ESP_ERR_TIMEOUT; }
    uint8_t bytes[5] = {0};
    for (size_t bit = 0; bit < 40; bit++) {
        dht11_pulse_t low = pulses[start + 2 * bit];
        dht11_pulse_t high = pulses[start + 2 * bit + 1];
        if (low.level != 0 || high.level != 1 || !within(low.us, 35, 65) ||
            !(within(high.us, 15, 40) || within(high.us, 55, 90))) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        bytes[bit / 8] = (uint8_t)((bytes[bit / 8] << 1) | (high.us >= 55));
    }
    uint8_t checksum = bytes[0] + bytes[1] + bytes[2] + bytes[3];
    if (checksum != bytes[4]) { return ESP_ERR_INVALID_CRC; }
    if (bytes[0] > 100 || bytes[2] > 50) { return ESP_ERR_INVALID_RESPONSE; }
    /* Preserve the existing integer telemetry format. */
    *out = (dht11_reading_t){.humidity = bytes[0], .temperature = bytes[2]};
    return ESP_OK;
}
