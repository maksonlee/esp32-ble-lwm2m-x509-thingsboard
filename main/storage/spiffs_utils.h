#pragma once

#include <stdbool.h>

bool spiffs_mount(void);
void spiffs_unmount(void);
char *spiffs_read_file(const char *path);
