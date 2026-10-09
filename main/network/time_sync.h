#pragma once
#include <stdbool.h>
/* Called only from the LwM2M worker. Never disables TLS checks on failure. */
bool time_sync_wait(void);
