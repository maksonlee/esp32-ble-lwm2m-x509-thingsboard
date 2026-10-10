#include "firmware_update.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "firmware_update";
#define BOOT_DEADLINE_US (180LL * 1000000)
#define JOURNAL_MAGIC 0x4f544131u
#define HEADER_SIZE (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) \
                     + sizeof(esp_app_desc_t))

enum { JOURNAL_IDLE, JOURNAL_FAILED, JOURNAL_ARMED, JOURNAL_SUCCESS };
typedef struct {
    uint32_t magic;
    uint32_t phase;
    uint32_t address;
    uint8_t sha256[32];
} update_journal_t;

static nvs_handle_t storage;
static bool initialized, receiving, handle_open, candidate_valid;
static esp_ota_handle_t handle;
static const esp_partition_t *target;
static esp_app_desc_t candidate;
static uint8_t header[HEADER_SIZE];
static size_t header_used, received;
static update_journal_t journal;
static firmware_update_status_t status;
static TaskHandle_t restart_task;
static esp_timer_handle_t boot_timer;
static bool pending_boot;
static int64_t boot_deadline;
static atomic_bool restart_requested;
static atomic_bool local_ready;

bool firmware_update_boot_pending(void) { return pending_boot; }
void firmware_update_local_ready(void) { atomic_store(&local_ready, true); }

static void restart_worker(void *argument)
{
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    /* Return the Execute response and give Anjay a chance to notify Updating. */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

void firmware_update_request_reboot(void)
{
    atomic_store(&restart_requested, true);
    xTaskNotifyGive(restart_task);
}

static void boot_timeout(void *argument)
{
    /* Only wake a worker: flash and restart do not run on the ESP timer task.
     * Resetting a PENDING_VERIFY image makes the IDF bootloader roll back. */
    firmware_update_request_reboot();
}

esp_err_t firmware_update_boot_guard_start(void)
{
    if (xTaskCreate(restart_worker, "ota_restart", 3072, NULL, 5,
                    &restart_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        return ESP_FAIL;
    }
    if (esp_ota_get_state_partition(running, &state) == ESP_OK
            && state == ESP_OTA_IMG_PENDING_VERIFY) {
        pending_boot = true;
        boot_deadline = esp_timer_get_time() + BOOT_DEADLINE_US;
        const esp_timer_create_args_t timer = {
            .callback = boot_timeout, .name = "ota_boot_deadline"
        };
        esp_err_t error = esp_timer_create(&timer, &boot_timer);
        if (error != ESP_OK) {
            return error;
        }
        ESP_LOGI(TAG, "Candidate must register within 180 seconds");
        return esp_timer_start_once(boot_timer, BOOT_DEADLINE_US);
    }
    return ESP_OK;
}

static esp_err_t save_journal(uint32_t phase)
{
    journal.magic = JOURNAL_MAGIC;
    journal.phase = phase;
    esp_err_t error = nvs_set_blob(storage, "attempt", &journal, sizeof(journal));
    return error == ESP_OK ? nvs_commit(storage) : error;
}

esp_err_t firmware_update_init(void)
{
    if (initialized) {
        return ESP_OK;
    }
    esp_err_t error = nvs_open("firmware_update", NVS_READWRITE, &storage);
    if (error != ESP_OK) {
        return error;
    }
    size_t length = sizeof(journal);
    error = nvs_get_blob(storage, "attempt", &journal, &length);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        memset(&journal, 0, sizeof(journal));
    } else if (error != ESP_OK || length != sizeof(journal)
            || journal.magic != JOURNAL_MAGIC || journal.phase > JOURNAL_SUCCESS) {
        nvs_close(storage);
        return ESP_FAIL;
    }
    status = FIRMWARE_UPDATE_IDLE;
    if (journal.phase == JOURNAL_ARMED) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        uint8_t digest[32];
        if (running && running->address == journal.address
                && esp_partition_get_sha256(running, digest) == ESP_OK
                && !memcmp(digest, journal.sha256, sizeof(digest))) {
            /* Even after power loss between mark-valid and journal commit,
             * wait for registration again before reporting success. */
            status = FIRMWARE_UPDATE_UPDATING;
        } else {
            status = FIRMWARE_UPDATE_FAILURE;
            error = save_journal(JOURNAL_FAILED);
        }
    } else if (journal.phase == JOURNAL_SUCCESS) {
        status = FIRMWARE_UPDATE_SUCCESS;
    } else if (journal.phase == JOURNAL_FAILED) {
        status = FIRMWARE_UPDATE_FAILURE;
    }
    if (pending_boot) {
        status = FIRMWARE_UPDATE_UPDATING;
    }
    if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(storage);
        return error;
    }
    initialized = true;
    return ESP_OK;
}

static void discard(void)
{
    if (handle_open) {
        esp_ota_abort(handle);
        handle_open = false;
    }
    receiving = candidate_valid = false;
    target = NULL;
    header_used = received = 0;
    memset(&candidate, 0, sizeof(candidate));
}

void firmware_update_reset(void)
{
    discard();
    status = FIRMWARE_UPDATE_IDLE;
    if (initialized && save_journal(JOURNAL_IDLE) != ESP_OK) {
        status = FIRMWARE_UPDATE_FAILURE;
    }
}

void firmware_update_disconnect(void)
{
    if (receiving) {
        discard();
        status = FIRMWARE_UPDATE_FAILURE;
        /* Begin already persisted failure, so power loss is also covered. */
    }
}

int firmware_update_begin(void)
{
    if (!initialized || pending_boot || status == FIRMWARE_UPDATE_UPDATING
            || receiving || candidate_valid || atomic_load(&restart_requested)) {
        return FIRMWARE_UPDATE_FAILED;
    }
    discard();
    target = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!target || !running || target->address == running->address) {
        return FIRMWARE_UPDATE_FAILED;
    }
    /* A reset during download must report failure, never false success. */
    if (save_journal(JOURNAL_FAILED) != ESP_OK) {
        return FIRMWARE_UPDATE_FAILED;
    }
    status = FIRMWARE_UPDATE_IDLE;
    receiving = true;
    return FIRMWARE_UPDATE_OK;
}

static int fail_transfer(int error)
{
    discard();
    status = FIRMWARE_UPDATE_FAILURE;
    return error;
}

static int validate_header(void)
{
    esp_image_header_t image;
    memcpy(&image, header, sizeof(image));
    memcpy(&candidate, header + sizeof(image) + sizeof(esp_image_segment_header_t),
           sizeof(candidate));
    const esp_app_desc_t *running = esp_app_get_description();
    if (image.magic != ESP_IMAGE_HEADER_MAGIC || image.chip_id != ESP_CHIP_ID_ESP32
            || !image.hash_appended || candidate.magic_word != ESP_APP_DESC_MAGIC_WORD
            || !memchr(candidate.project_name, 0, sizeof(candidate.project_name))
            || !memchr(candidate.version, 0, sizeof(candidate.version))
            || !candidate.version[0]
            || strcmp(candidate.project_name, running->project_name)
            || !strcmp(candidate.version, running->version)) {
        return FIRMWARE_UPDATE_UNSUPPORTED;
    }
    esp_err_t error = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (error != ESP_OK) {
        return error == ESP_ERR_NO_MEM ? FIRMWARE_UPDATE_NO_MEMORY
                                      : FIRMWARE_UPDATE_FAILED;
    }
    handle_open = true;
    return esp_ota_write(handle, header, sizeof(header)) == ESP_OK
            ? FIRMWARE_UPDATE_OK : FIRMWARE_UPDATE_FAILED;
}

int firmware_update_write(const void *data, size_t length)
{
    if (!receiving || !data || !length) {
        return FIRMWARE_UPDATE_FAILED;
    }
    if (length > target->size - received) {
        return fail_transfer(FIRMWARE_UPDATE_NO_SPACE);
    }
    received += length;
    const uint8_t *bytes = data;
    if (header_used < sizeof(header)) {
        size_t count = sizeof(header) - header_used;
        if (count > length) {
            count = length;
        }
        memcpy(header + header_used, bytes, count);
        header_used += count;
        bytes += count;
        length -= count;
        if (header_used == sizeof(header)) {
            int error = validate_header();
            if (error) {
                return fail_transfer(error);
            }
        }
    }
    if (length && esp_ota_write(handle, bytes, length) != ESP_OK) {
        return fail_transfer(FIRMWARE_UPDATE_FAILED);
    }
    return FIRMWARE_UPDATE_OK;
}

int firmware_update_finish(void)
{
    if (!receiving || !handle_open) {
        return fail_transfer(FIRMWARE_UPDATE_INTEGRITY);
    }
    esp_err_t error = esp_ota_end(handle);
    handle_open = false; /* IDF frees the handle on success AND failure. */
    esp_image_metadata_t metadata = {0};
    const esp_partition_pos_t position = { .offset = target->address,
                                          .size = target->size };
    if (error != ESP_OK
            || esp_image_verify(ESP_IMAGE_VERIFY, &position, &metadata) != ESP_OK
            || metadata.image_len != received) {
        return fail_transfer(FIRMWARE_UPDATE_INTEGRITY);
    }
    receiving = false;
    candidate_valid = true;
    status = FIRMWARE_UPDATE_DOWNLOADED;
    return FIRMWARE_UPDATE_OK;
}

int firmware_update_perform(void)
{
    if (!candidate_valid || status != FIRMWARE_UPDATE_DOWNLOADED) {
        return FIRMWARE_UPDATE_FAILED;
    }
    journal.address = target->address;
    if (esp_partition_get_sha256(target, journal.sha256) != ESP_OK
            || save_journal(JOURNAL_ARMED) != ESP_OK) {
        /* Anjay keeps Downloaded after an Execute failure; retain the
         * validated candidate so a later Execute can retry coherently. */
        return FIRMWARE_UPDATE_FAILED;
    }
    if (esp_ota_set_boot_partition(target) != ESP_OK) {
        save_journal(JOURNAL_FAILED);
        return FIRMWARE_UPDATE_FAILED;
    }
    status = FIRMWARE_UPDATE_UPDATING;
    firmware_update_request_reboot();
    return FIRMWARE_UPDATE_OK;
}

esp_err_t firmware_update_confirm(void)
{
    if (!initialized || !atomic_load(&local_ready) || atomic_load(&restart_requested)) {
        return ESP_FAIL;
    }
    if (pending_boot) {
        if (esp_timer_get_time() >= boot_deadline) {
            firmware_update_request_reboot();
            return ESP_FAIL;
        }
        /* Stop first; if confirmation fails, restart to trigger rollback. */
        if (esp_timer_stop(boot_timer) != ESP_OK
                || esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
            firmware_update_request_reboot();
            return ESP_FAIL;
        }
        pending_boot = false;
    }
    if (status == FIRMWARE_UPDATE_UPDATING) {
        esp_err_t error = save_journal(JOURNAL_SUCCESS);
        if (error != ESP_OK) {
            return error;
        }
        status = FIRMWARE_UPDATE_SUCCESS;
        ESP_LOGI(TAG, "New firmware confirmed after server registration");
    }
    return ESP_OK;
}

firmware_update_status_t firmware_update_status(void) { return status; }
const char *firmware_update_package_name(void)
{
    return candidate_valid ? candidate.project_name : NULL;
}
const char *firmware_update_package_version(void)
{
    return candidate_valid ? candidate.version : NULL;
}
const char *firmware_update_running_version(void)
{
    return esp_app_get_description()->version;
}
