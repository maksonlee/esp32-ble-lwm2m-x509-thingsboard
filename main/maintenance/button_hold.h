#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    bool armed, pressed, fired;
    int64_t since;
} button_hold_t;
bool button_hold_update(button_hold_t *state, bool down, int64_t now_us);
