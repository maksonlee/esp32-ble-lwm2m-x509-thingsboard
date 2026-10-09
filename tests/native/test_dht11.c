#include <assert.h>
#include "../../main/sensors/dht11_decode.c"

static dht11_pulse_t p[85];
static void frame(const uint8_t bytes[5]) {
    p[0] = (dht11_pulse_t){25, 1};
    p[1] = (dht11_pulse_t){80, 0}; p[2] = (dht11_pulse_t){80, 1};
    for (int bit = 0; bit < 40; bit++) {
        p[3 + bit * 2] = (dht11_pulse_t){50, 0};
        p[4 + bit * 2] = (dht11_pulse_t){(bytes[bit / 8] & (0x80 >> (bit % 8))) ? 70 : 27, 1};
    }
}
int main(void) {
    dht11_reading_t out = {.temperature = -99, .humidity = -99};
    uint8_t data[] = {49, 0, 23, 0, 72};
    frame(data); assert(dht11_decode(p, 83, &out) == ESP_OK);
    assert(out.temperature == 23 && out.humidity == 49);
    out.temperature = -99;
    assert(dht11_decode(p, 82, &out) == ESP_ERR_TIMEOUT);
    assert(out.temperature == -99);
    p[4].us = 47; assert(dht11_decode(p, 83, &out) == ESP_ERR_INVALID_RESPONSE);
    data[4]++; frame(data); assert(dht11_decode(p, 83, &out) == ESP_ERR_INVALID_CRC);
    data[0] = 120; data[4] = 143; frame(data);
    assert(dht11_decode(p, 83, &out) == ESP_ERR_INVALID_RESPONSE);
    assert(dht11_decode(NULL, 0, &out) == ESP_ERR_INVALID_ARG);
    frame(data); p[1].level = 1;
    assert(dht11_decode(p, 83, &out) == ESP_ERR_TIMEOUT);
    return 0;
}
