#pragma once
#include <stddef.h>
#include <stdint.h>
#include "dht11.h"
typedef struct { uint16_t us; uint8_t level; } dht11_pulse_t;
esp_err_t dht11_decode(const dht11_pulse_t *pulses, size_t count, dht11_reading_t *out);
