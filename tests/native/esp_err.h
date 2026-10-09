#pragma once
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_TIMEOUT 2
#define ESP_ERR_INVALID_RESPONSE 3
#define ESP_ERR_INVALID_CRC 4
#define ESP_ERR_NO_MEM 5
#include <assert.h>
#define ESP_ERROR_CHECK(x) assert((x) == ESP_OK)
#define esp_err_to_name(x) "test error"
typedef int esp_err_t;
