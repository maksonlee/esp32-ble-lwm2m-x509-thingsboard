#pragma once
#include <stdbool.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
typedef struct {
    const char *base_path;
    const char *partition_label;
    size_t max_files;
    bool format_if_mount_failed;
} esp_vfs_spiffs_conf_t;
int esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *conf);
int esp_vfs_spiffs_unregister(const char *label);
int esp_spiffs_info(const char *label, size_t *total, size_t *used);
