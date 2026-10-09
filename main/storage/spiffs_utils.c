#include "spiffs_utils.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "SPIFFS_UTIL";
static bool mounted;

/* Bound allocation even if a corrupt filesystem reports a bogus length. */
#define MAX_CERT_FILE_SIZE (16 * 1024)

bool spiffs_mount(void)
{
    if (mounted) {
        return true;
    }
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = false};

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to mount SPIFFS (%s)", esp_err_to_name(ret));
        return false;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get SPIFFS info (%s)", esp_err_to_name(ret));
        esp_vfs_spiffs_unregister(NULL);
        return false;
    }

    ESP_LOGI(TAG, "SPIFFS mounted: total=%zu, used=%zu", total, used);
    mounted = true;
    return true;
}

void spiffs_unmount(void)
{
    if (mounted) {
        esp_vfs_spiffs_unregister(NULL);
        mounted = false;
        ESP_LOGI(TAG, "SPIFFS unmounted");
    }
}

char *spiffs_read_file(const char *path)
{
    if (!path) {
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size <= 0 || size > MAX_CERT_FILE_SIZE || fseek(f, 0, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Invalid certificate file size or seek failure: %s", path);
        fclose(f);
        return NULL;
    }

    char *buf = malloc(size + 1);
    if (!buf)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for file: %s", path);
        fclose(f);
        return NULL;
    }

    size_t read_size = fread(buf, 1, (size_t)size, f);
    bool read_failed = read_size != (size_t)size || ferror(f);
    int close_result = fclose(f);
    if (read_failed || close_result != 0) {
        ESP_LOGE(TAG, "Incomplete certificate file read: %s", path);
        free(buf);
        return NULL;
    }
    buf[size] = '\0';

    ESP_LOGI(TAG, "Read %ld bytes from %s", size, path);
    return buf;
}
