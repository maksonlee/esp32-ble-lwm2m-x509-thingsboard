#include "cert_manager.h"
#include "spiffs_utils.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "cert_manager";

char *cert_ca = NULL;
char *cert_client = NULL;
char *key_client = NULL;

bool cert_manager_load(void)
{
    if (cert_ca && cert_client && key_client) {
        return true;
    }
    cert_manager_free();
    if (!spiffs_mount()) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS");
        return false;
    }

    cert_ca     = spiffs_read_file("/spiffs/root_ca.crt");
    cert_client = spiffs_read_file("/spiffs/device.crt");
    key_client  = spiffs_read_file("/spiffs/device.key");

    if (!cert_ca || !cert_client || !key_client) {
        ESP_LOGE(TAG, "Failed to load one or more certificates from SPIFFS");
        cert_manager_free();
        return false;
    }

    return true;
}

void cert_manager_free(void)
{
    if (cert_ca)     { free(cert_ca);     cert_ca = NULL; }
    if (cert_client) { free(cert_client); cert_client = NULL; }
    if (key_client) {
        /* Do not leave the private key in a released heap block. */
        volatile unsigned char *p = (volatile unsigned char *)key_client;
        size_t length = strlen(key_client);
        while (length--) { *p++ = 0; }
        free(key_client);
        key_client = NULL;
    }

    spiffs_unmount();
}
