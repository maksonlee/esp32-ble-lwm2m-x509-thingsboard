/* Loopback-only wire test. This executable is never installed on the board. */
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>
#include <anjay/security.h>
#include <anjay/server.h>
#include "lwm2m/lwm2m_sensor.h"

int main(int argc, char **argv)
{
    if (argc != 2 || strncmp(argv[1], "coap://127.0.0.1:", 17)) return 2;
    const anjay_lwm2m_version_config_t version = {
        .minimum_version = ANJAY_LWM2M_VERSION_1_1,
        .maximum_version = ANJAY_LWM2M_VERSION_1_1
    };
    anjay_configuration_t config = {
        .endpoint_name = "isolated-observe-test",
        .in_buffer_size = 4096, .out_buffer_size = 4096,
        .lwm2m_version_config = &version,
        .confirmable_notifications = true
    };
    anjay_t *anjay = anjay_new(&config);
    lwm2m_sensor_t sensors[2];
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
            || lwm2m_sensors_install(anjay, sensors)) return 1;
    const dht11_reading_t reading = {23, 49};
    for (;;) {
        fd_set input;
        FD_ZERO(&input); FD_SET(STDIN_FILENO, &input);
        struct timeval timeout = {0};
        if (select(STDIN_FILENO + 1, &input, NULL, NULL, &timeout) > 0) {
            char command[80];
            if (!fgets(command, sizeof(command), stdin) || command[0] == 'q') break;
            long long timestamp;
            if (sscanf(command, "sample %lld", &timestamp) == 1) {
                if (lwm2m_sensors_update(anjay, sensors, &reading, (time_t)timestamp)) return 1;
            } else if (command[0] == 'f') {
                lwm2m_sensors_invalidate(sensors);
            }
        }
        anjay_sched_run(anjay);
        anjay_serve_any(anjay, avs_time_duration_from_scalar(20, AVS_TIME_MS));
    }
    anjay_delete(anjay);
    return 0;
}
