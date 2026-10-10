idf_component_register(
    SRCS
        "app_main.c"
        "lwm2m/lwm2m_client_handler.c"
        "lwm2m/lwm2m_sensor.c"
        "lwm2m/lwm2m_firmware.c"
        "maintenance/button_hold.c"
        "maintenance/maintenance_button.c"
        "maintenance/firmware_update.c"
        "network/time_sync.c"
        "network/wifi_provisioning.c"
        "sensors/dht11.c"
        "sensors/dht11_decode.c"
        "storage/cert_manager.c"
        "storage/spiffs_utils.c"
    INCLUDE_DIRS
        "."
    PRIV_REQUIRES
        anjay
        app_update
        bootloader_support
        esp_driver_gpio
        esp_driver_rmt
        esp_event
        esp_netif
        lwip
        esp_rom
        esp_timer
        esp_wifi
        nvs_flash
        spiffs
)

if(NOT CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE OR NOT CONFIG_ANJAY_WITH_MODULE_FW_UPDATE
        OR CONFIG_ANJAY_WITH_DOWNLOADER OR CONFIG_ANJAY_WITHOUT_MODULE_FW_UPDATE_PUSH_MODE)
    message(FATAL_ERROR "OTA requires bootloader rollback and Anjay firmware PUSH; disable the downloader")
endif()
