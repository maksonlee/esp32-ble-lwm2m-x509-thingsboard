#include <assert.h>
#include "../../main/maintenance/button_hold.c"
int main(void) {
    button_hold_t state = {0};
    assert(!button_hold_update(&state, true, 0));
    assert(!button_hold_update(&state, true, 9000000));
    assert(!button_hold_update(&state, false, 10000000));
    assert(!button_hold_update(&state, true, 11000000));
    assert(!button_hold_update(&state, true, 15999999));
    assert(button_hold_update(&state, true, 16000000));
    assert(!button_hold_update(&state, true, 20000000));
    assert(!button_hold_update(&state, false, 21000000));
    assert(!button_hold_update(&state, true, 22000000));
    assert(!button_hold_update(&state, false, 23000000));
    assert(!button_hold_update(&state, true, 24000000));
    assert(!button_hold_update(&state, true, 28000000));
    assert(button_hold_update(&state, true, 29000000));
    return 0;
}
