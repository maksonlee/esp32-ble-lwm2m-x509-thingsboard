#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_spiffs.h"

static int fault, closes, unmounts;
static long file_size = 5;
static FILE *fake_open(const char *path, const char *mode) {
    (void)path; (void)mode;
    return fault == 1 ? NULL : (FILE *)1;
}
static int fake_seek(FILE *f, long offset, int origin) {
    (void)f; (void)offset;
    return (fault == 2 || (fault == 4 && origin == SEEK_SET)) ? -1 : 0;
}
static long fake_tell(FILE *f) { (void)f; return fault == 3 ? -1 : file_size; }
static size_t fake_read(void *p, size_t size, size_t n, FILE *f) {
    (void)size; (void)f;
    memset(p, 'x', n);
    return fault == 5 ? n - 1 : n;
}
static int fake_error(FILE *f) { (void)f; return fault == 6; }
static int fake_close(FILE *f) { (void)f; closes++; return fault == 7 ? -1 : 0; }
int esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *conf) {
    assert(!conf->format_if_mount_failed);
    return 0;
}
int esp_vfs_spiffs_unregister(const char *label) { (void)label; unmounts++; return 0; }
int esp_spiffs_info(const char *label, size_t *total, size_t *used) {
    (void)label; *total = 100; *used = 5; return fault == 8 ? -1 : 0;
}
#define fopen fake_open
#define fseek fake_seek
#define ftell fake_tell
#define fread fake_read
#define ferror fake_error
#define fclose fake_close
#include "../../main/storage/spiffs_utils.c"

int main(void) {
    assert(spiffs_read_file(NULL) == NULL);
    for (fault = 1; fault <= 7; fault++) {
        closes = 0;
        assert(spiffs_read_file("fixture") == NULL);
        assert(closes == (fault == 1 ? 0 : 1));
    }
    fault = 0;
    file_size = 0; assert(spiffs_read_file("fixture") == NULL);
    file_size = 16385; assert(spiffs_read_file("fixture") == NULL);
    file_size = 5;
    char *data = spiffs_read_file("fixture");
    assert(data && strcmp(data, "xxxxx") == 0); free(data);
    fault = 8; assert(!spiffs_mount()); assert(unmounts == 1);
    fault = 0; assert(spiffs_mount()); assert(spiffs_mount());
    spiffs_unmount(); spiffs_unmount(); assert(unmounts == 2);
    return 0;
}
