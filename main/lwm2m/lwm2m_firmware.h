#pragma once

#include <anjay/anjay.h>

int lwm2m_firmware_install(anjay_t *anjay);
void lwm2m_firmware_registered(anjay_t *anjay);
void lwm2m_firmware_disconnect(void);
