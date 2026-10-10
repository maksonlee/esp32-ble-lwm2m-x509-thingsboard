#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Owned by the LwM2M task, except for the independent boot deadline timer. */
typedef enum {
    FIRMWARE_UPDATE_OK = 0,
    FIRMWARE_UPDATE_FAILED = -1,
    FIRMWARE_UPDATE_NO_SPACE = -2,
    FIRMWARE_UPDATE_NO_MEMORY = -3,
    FIRMWARE_UPDATE_INTEGRITY = -5,
    FIRMWARE_UPDATE_UNSUPPORTED = -6
} firmware_update_error_t;

typedef enum {
    FIRMWARE_UPDATE_IDLE,
    FIRMWARE_UPDATE_DOWNLOADED,
    FIRMWARE_UPDATE_UPDATING,
    FIRMWARE_UPDATE_SUCCESS,
    FIRMWARE_UPDATE_FAILURE
} firmware_update_status_t;

/* Start before network initialization so an offline candidate also rolls back. */
esp_err_t firmware_update_boot_guard_start(void);
bool firmware_update_boot_pending(void);
void firmware_update_local_ready(void);
/* Call after NVS initialization. Idempotent across client reconnections. */
esp_err_t firmware_update_init(void);
int firmware_update_begin(void);
int firmware_update_write(const void *data, size_t length);
int firmware_update_finish(void);
void firmware_update_reset(void);
/* Discard an incomplete transfer; retain a validated package until Execute. */
void firmware_update_disconnect(void);
int firmware_update_perform(void);
/* After local startup and a fresh authenticated server registration. */
esp_err_t firmware_update_confirm(void);
firmware_update_status_t firmware_update_status(void);
const char *firmware_update_package_name(void);
const char *firmware_update_package_version(void);
const char *firmware_update_running_version(void);
/* Deferred so the protocol can acknowledge Execute before restarting. */
void firmware_update_request_reboot(void);
