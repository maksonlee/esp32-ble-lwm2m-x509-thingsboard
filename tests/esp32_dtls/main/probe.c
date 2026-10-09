#include <assert.h>
#include <stdio.h>

#include <anjay/anjay.h>
#include <anjay/security.h>
#include <anjay/server.h>
#include <esp_heap_caps.h>
#include <esp_netif.h>
#include "dtls_probe.h"

#ifdef NDEBUG
#error "This test requires assertions to execute and validate its checks"
#endif

void app_main(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    const anjay_configuration_t config = {
        .endpoint_name = "isolated-anjay-probe",
        .in_buffer_size = 2048,
        .out_buffer_size = 2048
    };
    anjay_t *anjay = anjay_new(&config);
    assert(anjay);
    assert(!anjay_security_object_install(anjay));
    assert(!anjay_server_object_install(anjay));
    anjay_delete(anjay);
    printf("PROBE: core initialization PASS; free heap=%u\n",
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT));
    run_dtls_probe();
    puts("PROBE: ALL PASS");
}
