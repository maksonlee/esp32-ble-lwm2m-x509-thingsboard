#include "button_hold.h"

bool button_hold_update(button_hold_t *state, bool down, int64_t now_us)
{
    if (!down) {
        state->armed = true;
        state->pressed = state->fired = false;
        return false;
    }
    /* Ignore a pin already held low at startup, including boot strapping. */
    if (!state->armed) { return false; }
    if (!state->pressed) {
        state->pressed = true;
        state->since = now_us;
    }
    if (!state->fired && now_us - state->since >= 5000000) {
        state->fired = true;
        return true;
    }
    return false;
}
