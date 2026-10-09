#pragma once

#include <stdbool.h>

extern char *cert_ca;
extern char *cert_client;
extern char *key_client;

bool cert_manager_load(void);
void cert_manager_free(void);