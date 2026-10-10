#pragma once

/* Narrow ESP-IDF substitutes for the actual firmware update module. The image
 * sizes/descriptor layout mirror the SDK; flash validation itself is injected. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_IMAGE_HEADER_MAGIC 0xe9
#define ESP_APP_DESC_MAGIC_WORD 0xabcd5432u
#define ESP_CHIP_ID_ESP32 0
#define OTA_WITH_SEQUENTIAL_WRITES ((size_t)-2)
#define NVS_READWRITE 1
#define ESP_IMAGE_VERIFY 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)

typedef struct {
    uint8_t magic, segment_count, spi_mode, spi_speed_size;
    uint32_t entry_addr;
    uint8_t wp_pin, spi_pin_drv[3];
    uint16_t chip_id;
    uint8_t min_chip_rev;
    uint16_t min_chip_rev_full, max_chip_rev_full;
    uint8_t reserved[4], hash_appended;
} __attribute__((packed)) esp_image_header_t;

typedef struct { uint32_t load_addr, data_len; } esp_image_segment_header_t;
typedef struct {
    uint32_t magic_word, secure_version, reserv1[2];
    char version[32], project_name[32], time[16], date[16], idf_ver[32];
    uint8_t app_elf_sha256[32];
    uint16_t min_efuse_blk_rev_full, max_efuse_blk_rev_full;
    uint8_t mmu_page_size, spi_flash_mode, reserv3[2];
    uint32_t reserv2[18];
} esp_app_desc_t;
_Static_assert(sizeof(esp_image_header_t) == 24, "SDK image header size");
_Static_assert(sizeof(esp_app_desc_t) == 256, "SDK descriptor size");

typedef struct { uint32_t address, size; } esp_partition_t;
typedef struct { uint32_t offset, size; } esp_partition_pos_t;
typedef struct { uint32_t image_len; } esp_image_metadata_t;
typedef enum {
    ESP_OTA_IMG_NEW, ESP_OTA_IMG_PENDING_VERIFY, ESP_OTA_IMG_VALID,
    ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED, ESP_OTA_IMG_UNDEFINED = -1
} esp_ota_img_states_t;
typedef unsigned esp_ota_handle_t;
typedef unsigned nvs_handle_t;
typedef void *TaskHandle_t;
typedef void *esp_timer_handle_t;
typedef struct { void (*callback)(void *); void *arg; const char *name; }
        esp_timer_create_args_t;
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)

const esp_app_desc_t *esp_app_get_description(void);
const esp_partition_t *esp_ota_get_running_partition(void);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *, esp_ota_img_states_t *);
esp_err_t esp_partition_get_sha256(const esp_partition_t *, uint8_t *);
esp_err_t esp_ota_begin(const esp_partition_t *, size_t, esp_ota_handle_t *);
esp_err_t esp_ota_write(esp_ota_handle_t, const void *, size_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_abort(esp_ota_handle_t);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
esp_err_t esp_image_verify(int, const esp_partition_pos_t *, esp_image_metadata_t *);
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *);
unsigned ulTaskNotifyTake(int, uint32_t);
void xTaskNotifyGive(TaskHandle_t);
void vTaskDelay(uint32_t);
void esp_restart(void) __attribute__((noreturn));
int64_t esp_timer_get_time(void);
esp_err_t esp_timer_create(const esp_timer_create_args_t *, esp_timer_handle_t *);
esp_err_t esp_timer_start_once(esp_timer_handle_t, uint64_t);
esp_err_t esp_timer_stop(esp_timer_handle_t);
