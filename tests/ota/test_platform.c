#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

/* Including the actual module permits a simulated cold boot to reset its RAM
 * while preserving the fake NVS and selected flash partition. */
#include "../../main/maintenance/firmware_update.c"

enum { IMAGE_SIZE = 640, FLASH_SIZE = 4096 };
enum { EVENT_COMMIT = 100, EVENT_BOOT = 200, EVENT_VALID = 300 };
static esp_partition_t slots[] = {
    { .address = 0x10000, .size = FLASH_SIZE },
    { .address = 0x1e0000, .size = FLASH_SIZE }
};
static unsigned running_slot;
static esp_app_desc_t running_description;
static esp_ota_img_states_t running_state;
static update_journal_t durable, staged;
static bool durable_present, staged_present, sdk_handle_live;
static uint8_t image[IMAGE_SIZE], flash_data[FLASH_SIZE];
static size_t flash_size;
static uint32_t verified_size;
static unsigned begins, writes, ends, aborts, boot_changes, marks, notices;
static unsigned timer_stops, closes, delays, restarts;
static unsigned events[32], event_count;
static esp_err_t open_error, get_error, set_error, commit_error, hash_error;
static esp_err_t begin_error, end_error, verify_error, boot_error, mark_error;
static esp_err_t timer_create_error, timer_start_error, timer_stop_error;
static unsigned write_error_at;
static bool task_error, same_target, absent_target, wrong_digest;
static int64_t now_us;
static uint64_t timeout_us;
static esp_timer_create_args_t timer_args;
static void (*worker_fn)(void *);
static jmp_buf restart_return;
static bool restart_escape;

static void event(unsigned value)
{
    assert(event_count < sizeof(events) / sizeof(events[0]));
    events[event_count++] = value;
}

const esp_app_desc_t *esp_app_get_description(void) { return &running_description; }
const esp_partition_t *esp_ota_get_running_partition(void) { return &slots[running_slot]; }
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *unused)
{
    (void)unused;
    return absent_target ? NULL : &slots[same_target ? running_slot : 1 - running_slot];
}
esp_err_t esp_ota_get_state_partition(const esp_partition_t *part, esp_ota_img_states_t *state)
{
    assert(part == &slots[running_slot]);
    *state = running_state;
    return ESP_OK;
}
esp_err_t esp_partition_get_sha256(const esp_partition_t *part, uint8_t *digest)
{
    memset(digest, (part == &slots[0] ? 0x30 : 0x60) + wrong_digest, 32);
    return hash_error;
}
esp_err_t esp_ota_begin(const esp_partition_t *part, size_t size, esp_ota_handle_t *out)
{
    ++begins;
    assert(part != &slots[running_slot]);
    assert(size == OTA_WITH_SEQUENTIAL_WRITES);
    assert(!sdk_handle_live);
    if (begin_error) { return begin_error; }
    sdk_handle_live = true;
    *out = 47;
    flash_size = 0;
    return ESP_OK;
}
esp_err_t esp_ota_write(esp_ota_handle_t value, const void *data, size_t size)
{
    assert(value == 47 && sdk_handle_live);
    ++writes;
    if (writes == write_error_at) { return ESP_FAIL; }
    assert(size <= sizeof(flash_data) - flash_size);
    memcpy(flash_data + flash_size, data, size);
    flash_size += size;
    return ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t value)
{
    assert(value == 47 && sdk_handle_live);
    sdk_handle_live = false; /* The real API frees even on error. */
    ++ends;
    return end_error;
}
esp_err_t esp_ota_abort(esp_ota_handle_t value)
{
    assert(value == 47 && sdk_handle_live);
    sdk_handle_live = false;
    ++aborts;
    return ESP_OK;
}
esp_err_t esp_image_verify(int mode, const esp_partition_pos_t *part,
                           esp_image_metadata_t *metadata)
{
    assert(mode == ESP_IMAGE_VERIFY && part->offset == slots[1 - running_slot].address);
    assert(part->size == FLASH_SIZE);
    metadata->image_len = verified_size;
    return verify_error;
}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *part)
{
    assert(!sdk_handle_live && part != &slots[running_slot]);
    /* A selected candidate must have a durable attempt record already. */
    assert(durable_present && durable.phase == JOURNAL_ARMED);
    assert(durable.address == part->address);
    event(EVENT_BOOT);
    ++boot_changes;
    return boot_error;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{
    event(EVENT_VALID);
    ++marks;
    if (!mark_error) { running_state = ESP_OTA_IMG_VALID; }
    return mark_error;
}

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *out)
{
    assert(!strcmp(name, "firmware_update") && mode == NVS_READWRITE);
    *out = 9;
    return open_error;
}
esp_err_t nvs_get_blob(nvs_handle_t value, const char *key, void *out, size_t *size)
{
    assert(value == 9 && !strcmp(key, "attempt") && *size == sizeof(durable));
    if (get_error) { return get_error; }
    if (!durable_present) { return ESP_ERR_NVS_NOT_FOUND; }
    memcpy(out, &durable, sizeof(durable));
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t value, const char *key, const void *data, size_t size)
{
    assert(value == 9 && !strcmp(key, "attempt") && size == sizeof(staged));
    if (set_error) { return set_error; }
    memcpy(&staged, data, size);
    staged_present = true;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t value)
{
    assert(value == 9 && staged_present);
    if (commit_error) { return commit_error; }
    durable = staged;
    durable_present = true;
    staged_present = false;
    event(EVENT_COMMIT + durable.phase);
    return ESP_OK;
}
void nvs_close(nvs_handle_t value) { assert(value == 9); ++closes; }

int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg,
                unsigned priority, TaskHandle_t *out)
{
    (void)name; (void)stack; (void)arg; (void)priority;
    if (task_error) { return 0; }
    worker_fn = fn;
    *out = (void *)1;
    return pdPASS;
}
unsigned ulTaskNotifyTake(int clear, uint32_t timeout)
{
    assert(clear == pdTRUE && timeout == portMAX_DELAY && notices > 0);
    return notices;
}
void xTaskNotifyGive(TaskHandle_t task) { assert(task == (void *)1); ++notices; }
void vTaskDelay(uint32_t value) { assert(value == 1000); ++delays; }
void esp_restart(void)
{
    ++restarts;
    assert(restart_escape);
    longjmp(restart_return, 1);
}
int64_t esp_timer_get_time(void) { return now_us; }
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out)
{
    timer_args = *args;
    *out = (void *)2;
    return timer_create_error;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t value, uint64_t timeout)
{
    assert(value == (void *)2);
    timeout_us = timeout;
    return timer_start_error;
}
esp_err_t esp_timer_stop(esp_timer_handle_t value)
{
    assert(value == (void *)2);
    ++timer_stops;
    return timer_stop_error;
}

static void reset_ram(void)
{
    storage = 0;
    initialized = receiving = handle_open = candidate_valid = false;
    handle = 0;
    target = NULL;
    memset(&candidate, 0, sizeof(candidate));
    memset(header, 0, sizeof(header));
    header_used = received = 0;
    memset(&journal, 0, sizeof(journal));
    status = FIRMWARE_UPDATE_IDLE;
    restart_task = boot_timer = NULL;
    pending_boot = false;
    boot_deadline = 0;
    atomic_store(&restart_requested, false);
    atomic_store(&local_ready, false);
    sdk_handle_live = staged_present = false;
    memset(&staged, 0, sizeof(staged));
    memset(&timer_args, 0, sizeof(timer_args));
    now_us = timeout_us = 0;
    worker_fn = NULL;
}

static void fixture(void)
{
    reset_ram();
    memset(&durable, 0, sizeof(durable));
    durable_present = false;
    running_slot = 0;
    running_state = ESP_OTA_IMG_VALID;
    memset(&running_description, 0, sizeof(running_description));
    strcpy(running_description.project_name, "wifi_prov_mgr");
    strcpy(running_description.version, "v01");
    running_description.magic_word = ESP_APP_DESC_MAGIC_WORD;
    memset(image, 0x5a, sizeof(image));
    esp_image_header_t image_header = {
        .magic = ESP_IMAGE_HEADER_MAGIC, .segment_count = 1,
        .chip_id = ESP_CHIP_ID_ESP32, .hash_appended = 1
    };
    esp_image_segment_header_t segment = { .data_len = IMAGE_SIZE - 32 };
    esp_app_desc_t desc = running_description;
    strcpy(desc.version, "v02");
    memcpy(image, &image_header, sizeof(image_header));
    memcpy(image + sizeof(image_header), &segment, sizeof(segment));
    memcpy(image + sizeof(image_header) + sizeof(segment), &desc, sizeof(desc));
    memset(flash_data, 0, sizeof(flash_data));
    flash_size = 0;
    verified_size = sizeof(image);
    begins = writes = ends = aborts = boot_changes = marks = notices = 0;
    timer_stops = closes = delays = restarts = event_count = 0;
    open_error = get_error = set_error = commit_error = hash_error = ESP_OK;
    begin_error = end_error = verify_error = boot_error = mark_error = ESP_OK;
    timer_create_error = timer_start_error = timer_stop_error = ESP_OK;
    write_error_at = 0;
    task_error = same_target = absent_target = wrong_digest = false;
    restart_escape = false;
}

static void boot(void)
{
    assert(firmware_update_boot_guard_start() == ESP_OK);
    assert(firmware_update_init() == ESP_OK);
    firmware_update_local_ready();
}

static void download(void)
{
    assert(firmware_update_begin() == FIRMWARE_UPDATE_OK);
    assert(firmware_update_write(image, sizeof(image)) == FIRMWARE_UPDATE_OK);
    assert(firmware_update_finish() == FIRMWARE_UPDATE_OK);
    assert(firmware_update_status() == FIRMWARE_UPDATE_DOWNLOADED);
}

static void reboot_into(unsigned slot, esp_ota_img_states_t state)
{
    reset_ram();
    running_slot = slot;
    running_state = state;
    boot();
}

static void test_headers(void)
{
    const size_t chunks[] = { 1, 23, 24, 31, 32, 287, 288, 512, IMAGE_SIZE };
    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); ++i) {
        fixture(); boot();
        assert(firmware_update_begin() == FIRMWARE_UPDATE_OK);
        for (size_t offset = 0; offset < sizeof(image);) {
            size_t count = chunks[i];
            if (count > sizeof(image) - offset) { count = sizeof(image) - offset; }
            assert(firmware_update_write(image + offset, count) == FIRMWARE_UPDATE_OK);
            offset += count;
            if (offset < HEADER_SIZE) { assert(begins == 0 && writes == 0); }
        }
        assert(firmware_update_finish() == FIRMWARE_UPDATE_OK);
        assert(begins == 1 && ends == 1 && flash_size == sizeof(image));
        assert(!memcmp(flash_data, image, sizeof(image)));
        assert(!strcmp(firmware_update_package_name(), "wifi_prov_mgr"));
        assert(!strcmp(firmware_update_package_version(), "v02"));
        assert(!strcmp(firmware_update_running_version(), "v01"));
    }
    for (unsigned kind = 0; kind < 9; ++kind) {
        fixture(); boot();
        esp_image_header_t ih;
        esp_app_desc_t desc;
        memcpy(&ih, image, sizeof(ih));
        memcpy(&desc, image + 32, sizeof(desc));
        switch (kind) {
        case 0: ih.magic = 0; break;
        case 1: ih.chip_id = 9; break;
        case 2: ih.hash_appended = 0; break;
        case 3: desc.magic_word = 0; break;
        case 4: strcpy(desc.project_name, "other_product"); break;
        case 5: memset(desc.project_name, 'p', sizeof(desc.project_name)); break;
        case 6: memset(desc.version, 'v', sizeof(desc.version)); break;
        case 7: desc.version[0] = 0; break;
        case 8: strcpy(desc.version, "v01"); break;
        }
        memcpy(image, &ih, sizeof(ih));
        memcpy(image + 32, &desc, sizeof(desc));
        assert(firmware_update_begin() == FIRMWARE_UPDATE_OK);
        assert(firmware_update_write(image, HEADER_SIZE - 1) == FIRMWARE_UPDATE_OK);
        assert(begins == 0);
        assert(firmware_update_write(image + HEADER_SIZE - 1, 1)
               == FIRMWARE_UPDATE_UNSUPPORTED);
        assert(begins == 0 && boot_changes == 0 && !sdk_handle_live);
        assert(firmware_update_status() == FIRMWARE_UPDATE_FAILURE);
        assert(firmware_update_package_version() == NULL);
    }
}

static void test_image(void)
{
    fixture(); boot();
    assert(firmware_update_begin() == 0);
    assert(firmware_update_write(image, SIZE_MAX) == FIRMWARE_UPDATE_NO_SPACE);
    assert(begins == 0 && boot_changes == 0);
    assert(firmware_update_begin() == 0);
    assert(firmware_update_write(image, HEADER_SIZE - 1) == 0);
    assert(firmware_update_finish() == FIRMWARE_UPDATE_INTEGRITY);
    assert(!sdk_handle_live && ends == 0);
    for (unsigned kind = 0; kind < 4; ++kind) {
        fixture(); boot();
        assert(firmware_update_begin() == 0);
        assert(firmware_update_write(image, sizeof(image)) == 0);
        if (kind == 0) { verified_size = sizeof(image) + 1; } /* Old flash tail. */
        if (kind == 1) { verified_size = sizeof(image) - 1; } /* Extra bytes. */
        if (kind == 2) { end_error = ESP_FAIL; }
        if (kind == 3) { verify_error = ESP_FAIL; }
        assert(firmware_update_finish() == FIRMWARE_UPDATE_INTEGRITY);
        assert(!sdk_handle_live && ends == 1 && aborts == 0 && boot_changes == 0);
        assert(firmware_update_perform() == FIRMWARE_UPDATE_FAILED);
        verified_size = sizeof(image); end_error = verify_error = ESP_OK;
        download(); /* Failed finish must not require an Anjay reset callback. */
        firmware_update_reset();
    }
}

static void test_flash(void)
{
    for (unsigned kind = 0; kind < 4; ++kind) {
        fixture(); boot();
        if (kind < 2) { begin_error = kind == 0 ? ESP_ERR_NO_MEM : ESP_FAIL; }
        else { write_error_at = kind - 1; }
        assert(firmware_update_begin() == 0);
        assert(firmware_update_write(image, sizeof(image))
               == (kind == 0 ? FIRMWARE_UPDATE_NO_MEMORY : FIRMWARE_UPDATE_FAILED));
        assert(!sdk_handle_live && boot_changes == 0);
        assert(aborts == (kind < 2 ? 0 : 1));
        begin_error = ESP_OK; write_error_at = 0;
        download();
        firmware_update_reset();
        assert(!sdk_handle_live);
    }
    fixture(); boot(); same_target = true;
    assert(firmware_update_begin() == FIRMWARE_UPDATE_FAILED);
    assert(begins == 0);
    same_target = false; absent_target = true;
    assert(firmware_update_begin() == FIRMWARE_UPDATE_FAILED);
    assert(begins == 0);
}

static void test_lifecycle(void)
{
    fixture(); boot();
    assert(firmware_update_begin() == 0);
    assert(firmware_update_write(image, HEADER_SIZE + 1) == 0);
    assert(firmware_update_begin() == FIRMWARE_UPDATE_FAILED);
    firmware_update_disconnect();
    assert(aborts == 1 && !sdk_handle_live);
    assert(firmware_update_status() == FIRMWARE_UPDATE_FAILURE);
    assert(durable.phase == JOURNAL_FAILED);
    download();
    firmware_update_disconnect();
    assert(firmware_update_status() == FIRMWARE_UPDATE_DOWNLOADED);
    assert(firmware_update_package_version() != NULL);
    firmware_update_reset();
    assert(firmware_update_status() == FIRMWARE_UPDATE_IDLE);
    assert(durable.phase == JOURNAL_IDLE && firmware_update_package_name() == NULL);
    download();
    assert(firmware_update_perform() == 0);
    assert(events[event_count - 2] == EVENT_COMMIT + JOURNAL_ARMED);
    assert(events[event_count - 1] == EVENT_BOOT);
    assert(boot_changes == 1 && notices == 1 && firmware_update_status() == FIRMWARE_UPDATE_UPDATING);
    assert(firmware_update_begin() == FIRMWARE_UPDATE_FAILED);
    assert(firmware_update_perform() == FIRMWARE_UPDATE_FAILED);

    for (unsigned kind = 0; kind < 4; ++kind) {
        fixture(); boot(); download();
        if (kind == 0) { hash_error = ESP_FAIL; }
        if (kind == 1) { set_error = ESP_FAIL; }
        if (kind == 2) { commit_error = ESP_FAIL; }
        if (kind == 3) { boot_error = ESP_FAIL; }
        assert(firmware_update_perform() == FIRMWARE_UPDATE_FAILED);
        assert(boot_changes == (kind == 3 ? 1 : 0) && notices == 0);
        assert(!sdk_handle_live && firmware_update_status() == FIRMWARE_UPDATE_DOWNLOADED);
        assert(durable.phase == JOURNAL_FAILED);
        assert(firmware_update_package_version() != NULL);
        hash_error = set_error = commit_error = boot_error = ESP_OK;
        assert(firmware_update_perform() == FIRMWARE_UPDATE_OK);
        assert(firmware_update_status() == FIRMWARE_UPDATE_UPDATING && notices == 1);
    }
}

static void arm_attempt(void)
{
    fixture(); boot(); download();
    assert(firmware_update_perform() == 0);
    assert(durable.phase == JOURNAL_ARMED);
}

static void test_journal(void)
{
    fixture(); boot();
    assert(firmware_update_begin() == 0);
    assert(firmware_update_write(image, HEADER_SIZE + 1) == 0);
    reboot_into(0, ESP_OTA_IMG_VALID);
    assert(firmware_update_status() == FIRMWARE_UPDATE_FAILURE);
    download();
    reboot_into(0, ESP_OTA_IMG_VALID);
    assert(firmware_update_status() == FIRMWARE_UPDATE_FAILURE);

    arm_attempt();
    reboot_into(0, ESP_OTA_IMG_VALID); /* Both pre-switch power loss and rollback. */
    assert(firmware_update_status() == FIRMWARE_UPDATE_FAILURE);
    assert(durable.phase == JOURNAL_FAILED);
    arm_attempt();
    wrong_digest = true;
    reboot_into(1, ESP_OTA_IMG_VALID);
    assert(firmware_update_status() == FIRMWARE_UPDATE_FAILURE);

    arm_attempt();
    reboot_into(1, ESP_OTA_IMG_PENDING_VERIFY);
    assert(firmware_update_status() == FIRMWARE_UPDATE_UPDATING && marks == 0);
    assert(firmware_update_confirm() == ESP_OK);
    assert(events[event_count - 2] == EVENT_VALID);
    assert(events[event_count - 1] == EVENT_COMMIT + JOURNAL_SUCCESS);
    assert(firmware_update_status() == FIRMWARE_UPDATE_SUCCESS && !pending_boot);
    assert(firmware_update_init() == ESP_OK); /* Recreating Anjay does not consume result. */
    assert(firmware_update_status() == FIRMWARE_UPDATE_SUCCESS);
    reboot_into(1, ESP_OTA_IMG_VALID);
    assert(firmware_update_status() == FIRMWARE_UPDATE_SUCCESS && timeout_us == 0);

    arm_attempt();
    reboot_into(1, ESP_OTA_IMG_PENDING_VERIFY);
    commit_error = ESP_FAIL;
    assert(firmware_update_confirm() == ESP_FAIL);
    assert(running_state == ESP_OTA_IMG_VALID && durable.phase == JOURNAL_ARMED);
    commit_error = ESP_OK;
    reboot_into(1, ESP_OTA_IMG_VALID); /* Power loss after mark-valid, before commit. */
    assert(firmware_update_status() == FIRMWARE_UPDATE_UPDATING);
    assert(firmware_update_confirm() == ESP_OK);
    assert(firmware_update_status() == FIRMWARE_UPDATE_SUCCESS);

    fixture(); durable_present = true; durable.magic = 7;
    assert(firmware_update_init() == ESP_FAIL && closes == 1 && !initialized);
    fixture(); open_error = ESP_FAIL;
    assert(firmware_update_init() == ESP_FAIL && !initialized);
    fixture(); get_error = ESP_FAIL;
    assert(firmware_update_init() == ESP_FAIL && closes == 1 && !initialized);
    fixture(); boot(); set_error = ESP_FAIL;
    assert(firmware_update_begin() == FIRMWARE_UPDATE_FAILED && begins == 0);
}

static void test_guard(void)
{
    fixture(); running_state = ESP_OTA_IMG_UNDEFINED; boot();
    assert(!pending_boot && timeout_us == 0); /* Initial USB image has no candidate. */
    fixture(); task_error = true;
    assert(firmware_update_boot_guard_start() == ESP_ERR_NO_MEM);
    for (unsigned kind = 0; kind < 2; ++kind) {
        fixture(); running_state = ESP_OTA_IMG_PENDING_VERIFY;
        if (kind == 0) { timer_create_error = ESP_FAIL; }
        else { timer_start_error = ESP_FAIL; }
        assert(firmware_update_boot_guard_start() == ESP_FAIL && marks == 0);
    }

    fixture(); running_state = ESP_OTA_IMG_PENDING_VERIFY;
    assert(firmware_update_boot_guard_start() == ESP_OK);
    assert(timeout_us == 180000000 && pending_boot);
    assert(firmware_update_confirm() == ESP_FAIL); /* Local init has not passed. */
    assert(firmware_update_init() == ESP_OK);
    assert(firmware_update_confirm() == ESP_FAIL); /* Application startup is incomplete. */
    firmware_update_local_ready();
    assert(firmware_update_begin() == FIRMWARE_UPDATE_FAILED && begins == 0);
    now_us = 179999999;
    assert(firmware_update_confirm() == ESP_OK && marks == 1 && timer_stops == 1);
    assert(firmware_update_status() == FIRMWARE_UPDATE_SUCCESS);

    fixture(); running_state = ESP_OTA_IMG_PENDING_VERIFY; boot();
    now_us = 180000000;
    assert(firmware_update_confirm() == ESP_FAIL && marks == 0 && notices == 1);
    assert(pending_boot);
    fixture(); running_state = ESP_OTA_IMG_PENDING_VERIFY; boot();
    now_us = 180000000;
    timer_args.callback(timer_args.arg); /* No Wi-Fi, SNTP, certificate or Anjay calls. */
    assert(notices == 1 && marks == 0 && restarts == 0);
    assert(firmware_update_confirm() == ESP_FAIL);
    restart_escape = true;
    if (!setjmp(restart_return)) { worker_fn(NULL); assert(false); }
    assert(delays == 1 && restarts == 1 && running_state == ESP_OTA_IMG_PENDING_VERIFY);

    for (unsigned kind = 0; kind < 2; ++kind) {
        fixture(); running_state = ESP_OTA_IMG_PENDING_VERIFY; boot();
        if (kind == 0) { timer_stop_error = ESP_FAIL; }
        else { mark_error = ESP_FAIL; }
        assert(firmware_update_confirm() == ESP_FAIL);
        assert(notices == 1 && pending_boot && durable.phase != JOURNAL_SUCCESS);
        assert(marks == (kind == 0 ? 0 : 1));
        assert(firmware_update_confirm() == ESP_FAIL);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "headers")) { test_headers(); }
    else if (!strcmp(argv[1], "image")) { test_image(); }
    else if (!strcmp(argv[1], "flash")) { test_flash(); }
    else if (!strcmp(argv[1], "lifecycle")) { test_lifecycle(); }
    else if (!strcmp(argv[1], "journal")) { test_journal(); }
    else if (!strcmp(argv[1], "guard")) { test_guard(); }
    else { assert(false); }
    printf("OTA platform %s: PASS\n", argv[1]);
    return 0;
}
