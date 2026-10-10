/* Loopback wire test of the real application bridge with a fake flash boundary.
 * No ESP-IDF image, physical flash, device identity or live server is accessed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include <anjay/security.h>
#include <anjay/server.h>
#include "lwm2m/lwm2m_firmware.h"
#include "maintenance/firmware_update.h"

static firmware_update_status_t status;
static bool receiving;
static const char *mode;
static unsigned opens, writes, finishes, upgrades, resets;
static unsigned char image[4096];
static size_t image_size;

esp_err_t firmware_update_init(void) { return ESP_OK; }
esp_err_t firmware_update_boot_guard_start(void) { return ESP_OK; }

int firmware_update_begin(void)
{
    ++opens;
    image_size = 0;
    status = FIRMWARE_UPDATE_IDLE;
    receiving = true;
    return FIRMWARE_UPDATE_OK;
}

int firmware_update_write(const void *data, size_t length)
{
    ++writes;
    if (!strcmp(mode, "space") || length > sizeof(image) - image_size) {
        status = FIRMWARE_UPDATE_FAILURE;
        receiving = false;
        image_size = 0;
        return FIRMWARE_UPDATE_NO_SPACE;
    }
    if (!strcmp(mode, "memory")) {
        status = FIRMWARE_UPDATE_FAILURE;
        receiving = false;
        image_size = 0;
        return FIRMWARE_UPDATE_NO_MEMORY;
    }
    memcpy(image + image_size, data, length);
    image_size += length;
    return FIRMWARE_UPDATE_OK;
}

int firmware_update_finish(void)
{
    ++finishes;
    receiving = false;
    if (!strcmp(mode, "bad") || !strcmp(mode, "unsupported")) {
        status = FIRMWARE_UPDATE_FAILURE;
        image_size = 0;
        return !strcmp(mode, "bad") ? FIRMWARE_UPDATE_INTEGRITY
                                    : FIRMWARE_UPDATE_UNSUPPORTED;
    }
    status = FIRMWARE_UPDATE_DOWNLOADED;
    return FIRMWARE_UPDATE_OK;
}

void firmware_update_reset(void)
{
    ++resets;
    status = FIRMWARE_UPDATE_IDLE;
    receiving = false;
    image_size = 0;
}

void firmware_update_disconnect(void)
{
    if (receiving) {
        status = FIRMWARE_UPDATE_FAILURE;
        image_size = 0;
        receiving = false;
    }
}

int firmware_update_perform(void)
{
    ++upgrades;
    if (!strcmp(mode, "upgrade-failure")) return FIRMWARE_UPDATE_FAILED;
    status = FIRMWARE_UPDATE_UPDATING;
    return FIRMWARE_UPDATE_OK;
}

esp_err_t firmware_update_confirm(void)
{
    if (!strcmp(mode, "confirm-failure")) return ESP_FAIL;
    if (status == FIRMWARE_UPDATE_UPDATING) status = FIRMWARE_UPDATE_SUCCESS;
    return ESP_OK;
}

void firmware_update_request_reboot(void) {}
firmware_update_status_t firmware_update_status(void) { return status; }
const char *firmware_update_package_name(void) { return "isolated-firmware-test"; }
const char *firmware_update_package_version(void) { return "v02"; }
const char *firmware_update_running_version(void) { return "v01"; }

static void print_stats(void)
{
    static const char *const names[] = {"idle", "downloaded", "updating", "success", "failure"};
    printf("{\"opens\":%u,\"writes\":%u,\"finishes\":%u,\"upgrades\":%u,"
           "\"resets\":%u,\"size\":%zu,\"status\":\"%s\",\"hex\":\"",
           opens, writes, finishes, upgrades, resets, image_size, names[status]);
    for (size_t i = 0; i < image_size; ++i) printf("%02x", image[i]);
    puts("\"}");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc != 4 || strncmp(argv[1], "coap://127.0.0.1:", 17)) return 2;
    setvbuf(stdin, NULL, _IONBF, 0);
    switch (atoi(argv[2])) {
    case 0: status = FIRMWARE_UPDATE_IDLE; break;
    case 1: status = FIRMWARE_UPDATE_SUCCESS; break;
    case 8: status = FIRMWARE_UPDATE_FAILURE; break;
    case -2: status = FIRMWARE_UPDATE_DOWNLOADED; break;
    case -3: status = FIRMWARE_UPDATE_UPDATING; break;
    default: return 2;
    }
    mode = argv[3];
    const anjay_lwm2m_version_config_t version = {
        .minimum_version = ANJAY_LWM2M_VERSION_1_1,
        .maximum_version = ANJAY_LWM2M_VERSION_1_1
    };
    const anjay_configuration_t config = {
        .endpoint_name = "isolated-firmware-test",
        .in_buffer_size = 4096, .out_buffer_size = 4096,
        .lwm2m_version_config = &version,
        .confirmable_notifications = true
    };
    anjay_t *anjay = anjay_new(&config);
    const anjay_security_instance_t security = {
        .ssid = 123, .server_uri = argv[1], .security_mode = ANJAY_SECURITY_NOSEC
    };
    const anjay_server_instance_t server = {
        .ssid = 123, .lifetime = 300, .default_min_period = 0,
        .default_max_period = -1, .disable_timeout = -1, .binding = "U"
    };
    anjay_iid_t security_iid = ANJAY_ID_INVALID, server_iid = ANJAY_ID_INVALID;
    if (!anjay || anjay_security_object_install(anjay)
            || anjay_server_object_install(anjay)
            || anjay_security_object_add_instance(anjay, &security, &security_iid)
            || anjay_server_object_add_instance(anjay, &server, &server_iid)
            || lwm2m_firmware_install(anjay)) return 1;
    for (;;) {
        fd_set input;
        FD_ZERO(&input);
        FD_SET(STDIN_FILENO, &input);
        struct timeval timeout = {0};
        if (select(STDIN_FILENO + 1, &input, NULL, NULL, &timeout) > 0) {
            char command[80];
            if (!fgets(command, sizeof(command), stdin) || command[0] == 'q') break;
            if (!strncmp(command, "stats", 5)) print_stats();
            else if (!strncmp(command, "good", 4)) mode = "good";
            else if (!strncmp(command, "registered", 10)) lwm2m_firmware_registered(anjay);
            else if (!strncmp(command, "short-timeout", 13)) {
                if (avs_is_err(anjay_update_coap_exchange_timeout(
                        anjay, ANJAY_TRANSPORT_SET_UDP,
                        avs_time_duration_from_scalar(100, AVS_TIME_MS)))) return 1;
            }
        }
        anjay_sched_run(anjay);
        anjay_serve_any(anjay, avs_time_duration_from_scalar(20, AVS_TIME_MS));
    }
    anjay_delete(anjay);
    lwm2m_firmware_disconnect();
    return 0;
}
