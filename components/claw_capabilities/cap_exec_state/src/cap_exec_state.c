/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "cap_exec_state.h"
#include "cap_exec_state_priv.h"
#include "claw_paths.h"
#include "claw_utils_string.h"
#include "claw_utils_time.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "cap_exec_state";

#define EXEC_STATE_DEFAULT_SUBPATH      "state/exec_state.json"
#define EXEC_STATE_PATH_MAX             160
#define EXEC_STATE_TMP_SUFFIX           ".tmp"
#define EXEC_STATE_BAD_SUFFIX           ".bad"
#define EXEC_STATE_FLUSH_RETRY_MS       2000
#define EXEC_STATE_FLUSH_TASK_NAME      "exec_state_flush"
#define EXEC_STATE_FLUSH_TASK_STACK     4096
#define EXEC_STATE_FLUSH_TASK_PRIO      (tskIDLE_PRIORITY + 2)
#define EXEC_STATE_JSON_KEY_VERSION     "version"
#define EXEC_STATE_JSON_KEY_ENTRIES     "entries"

struct cap_exec_state_t {
    char path[EXEC_STATE_PATH_MAX];
    char tmp_path[EXEC_STATE_PATH_MAX + sizeof(EXEC_STATE_TMP_SUFFIX)];
    char bad_path[EXEC_STATE_PATH_MAX + sizeof(EXEC_STATE_BAD_SUFFIX)];
    uint32_t max_entries;
    uint32_t max_file_bytes;
    int32_t flush_delay_ms;
    cap_exec_state_entry_t *entries;    /* oldest first, newest last */
    size_t count;
    SemaphoreHandle_t lock;             /* protects entries/count/dirty */
    SemaphoreHandle_t flush_lock;       /* serializes flushes; always taken before lock */
    SemaphoreHandle_t task_done;
    bool dirty;
    bool started;
    volatile bool stopping;
    esp_timer_handle_t flush_timer;
    TaskHandle_t flush_task;
};

typedef enum {
    EXEC_STATE_LOAD_OK,
    EXEC_STATE_LOAD_MISSING,
    EXEC_STATE_LOAD_BAD,
} exec_state_load_result_t;

/* ---------- helpers ---------- */

static void utf8_truncate_copy(char *dst, size_t dst_size, const char *src, size_t max_bytes)
{
    size_t len = 0;

    if (src) {
        size_t limit = max_bytes < dst_size - 1 ? max_bytes : dst_size - 1;
        len = claw_utils_utf8_prefix_len(src, limit);
        memcpy(dst, src, len);
    }
    dst[len] = '\0';
}

static int find_index(const struct cap_exec_state_t *h, const char *key)
{
    for (size_t i = 0; i < h->count; i++) {
        if (strcmp(h->entries[i].key, key) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void move_to_tail(struct cap_exec_state_t *h, size_t index)
{
    cap_exec_state_entry_t tmp;

    if (index + 1 >= h->count) {
        return;
    }
    tmp = h->entries[index];
    memmove(&h->entries[index], &h->entries[index + 1],
            (h->count - index - 1) * sizeof(h->entries[0]));
    h->entries[h->count - 1] = tmp;
}

static void evict_head(struct cap_exec_state_t *h, size_t n)
{
    if (n >= h->count) {
        h->count = 0;
        return;
    }
    memmove(&h->entries[0], &h->entries[n], (h->count - n) * sizeof(h->entries[0]));
    h->count -= n;
}

static bool key_is_valid(const char *key)
{
    size_t len = key ? strlen(key) : 0;

    return len > 0 && len <= CAP_EXEC_STATE_KEY_MAX_LEN;
}

const char *cap_exec_state_status_to_str(cap_exec_state_status_t status)
{
    switch (status) {
    case CAP_EXEC_STATE_STATUS_COMPLETED:
        return "completed";
    case CAP_EXEC_STATE_STATUS_FAILED:
        return "failed";
    case CAP_EXEC_STATE_STATUS_SKIPPED:
        return "skipped";
    default:
        return "unknown";
    }
}

esp_err_t cap_exec_state_status_from_str(const char *str, cap_exec_state_status_t *out_status)
{
    if (!str || !out_status) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int s = 0; s < CAP_EXEC_STATE_STATUS_MAX; s++) {
        if (strcmp(str, cap_exec_state_status_to_str((cap_exec_state_status_t)s)) == 0) {
            *out_status = (cap_exec_state_status_t)s;
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

/* ---------- load ---------- */

static double json_number_or(const cJSON *obj, const char *name, double fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);

    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static const char *json_string_or_empty(const cJSON *obj, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);

    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
}

/* Inserts one parsed item; the last duplicate wins and over-capacity trims the oldest. */
static void load_item_locked(struct cap_exec_state_t *h, const cJSON *item)
{
    const cJSON *key_item = cJSON_GetObjectItemCaseSensitive(item, "key");
    cap_exec_state_entry_t entry = {0};
    cap_exec_state_status_t status;
    double run_count;
    int index;

    if (!cJSON_IsObject(item) || !cJSON_IsString(key_item) || !key_is_valid(key_item->valuestring)) {
        return;
    }
    strlcpy(entry.key, key_item->valuestring, sizeof(entry.key));
    utf8_truncate_copy(entry.kind, sizeof(entry.kind), json_string_or_empty(item, "kind"),
                       CAP_EXEC_STATE_KIND_MAX_LEN);
    utf8_truncate_copy(entry.summary, sizeof(entry.summary), json_string_or_empty(item, "summary"),
                       CAP_EXEC_STATE_SUMMARY_MAX_LEN);
    if (cap_exec_state_status_from_str(json_string_or_empty(item, "status"), &status) != ESP_OK) {
        status = CAP_EXEC_STATE_STATUS_FAILED;
    }
    entry.status = status;
    entry.last_run_ms = (int64_t)json_number_or(item, "last_run_ms", 0);
    entry.first_seen_ms = (int64_t)json_number_or(item, "first_seen_ms", (double)entry.last_run_ms);
    run_count = json_number_or(item, "run_count", 1);
    if (run_count < 1) {
        run_count = 1;
    } else if (run_count > (double)UINT32_MAX) {
        run_count = (double)UINT32_MAX;
    }
    entry.run_count = (uint32_t)run_count;

    index = find_index(h, entry.key);
    if (index >= 0) {
        h->entries[index] = entry;
        move_to_tail(h, (size_t)index);
        return;
    }
    if (h->count >= h->max_entries) {
        evict_head(h, 1);
        h->dirty = true;
    }
    h->entries[h->count++] = entry;
}

static bool parse_state_locked(struct cap_exec_state_t *h, const char *text)
{
    cJSON *root = cJSON_Parse(text);
    const cJSON *version = NULL;
    const cJSON *entries = NULL;
    const cJSON *item = NULL;

    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    version = cJSON_GetObjectItemCaseSensitive(root, EXEC_STATE_JSON_KEY_VERSION);
    entries = cJSON_GetObjectItemCaseSensitive(root, EXEC_STATE_JSON_KEY_ENTRIES);
    if (!cJSON_IsNumber(version) || version->valuedouble > CAP_EXEC_STATE_FORMAT_VERSION ||
            !cJSON_IsArray(entries)) {
        cJSON_Delete(root);
        return false;
    }
    cJSON_ArrayForEach(item, entries) {
        load_item_locked(h, item);
    }
    cJSON_Delete(root);
    return true;
}

static exec_state_load_result_t load_file_locked(struct cap_exec_state_t *h, const char *path)
{
    struct stat st;
    FILE *fp = NULL;
    char *text = NULL;
    size_t read_len;
    bool parsed;

    if (stat(path, &st) != 0) {
        return EXEC_STATE_LOAD_MISSING;
    }
    if (st.st_size < 0 || (uint32_t)st.st_size > h->max_file_bytes) {
        ESP_LOGW(TAG, "State file %s too large (%ld bytes)", path, (long)st.st_size);
        return EXEC_STATE_LOAD_BAD;
    }
    fp = fopen(path, "rb");
    if (!fp) {
        return EXEC_STATE_LOAD_MISSING;
    }
    text = malloc((size_t)st.st_size + 1);
    if (!text) {
        fclose(fp);
        return EXEC_STATE_LOAD_BAD;
    }
    read_len = fread(text, 1, (size_t)st.st_size, fp);
    fclose(fp);
    text[read_len] = '\0';

    h->count = 0;
    parsed = read_len == (size_t)st.st_size && parse_state_locked(h, text);
    free(text);
    if (!parsed) {
        h->count = 0;
        return EXEC_STATE_LOAD_BAD;
    }
    return EXEC_STATE_LOAD_OK;
}

static void ensure_parent_dir(const char *path)
{
    char dir[EXEC_STATE_PATH_MAX];
    char *slash = NULL;

    strlcpy(dir, path, sizeof(dir));
    slash = strrchr(dir, '/');
    if (!slash || slash == dir) {
        return;
    }
    *slash = '\0';
    if (mkdir(dir, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s failed: errno=%d", dir, errno);
    }
}

static void quarantine_bad_file(struct cap_exec_state_t *h)
{
    ESP_LOGW(TAG, "Corrupt state file %s, moving to %s", h->path, h->bad_path);
    unlink(h->bad_path);
    if (rename(h->path, h->bad_path) != 0) {
        unlink(h->path);
    }
}

static void load_state(struct cap_exec_state_t *h)
{
    exec_state_load_result_t res;

    ensure_parent_dir(h->path);
    res = load_file_locked(h, h->path);
    if (res == EXEC_STATE_LOAD_BAD) {
        quarantine_bad_file(h);
    } else if (res == EXEC_STATE_LOAD_MISSING) {
        /* A crash between unlink(path) and rename(tmp, path) leaves only the tmp file. */
        if (load_file_locked(h, h->tmp_path) == EXEC_STATE_LOAD_OK) {
            ESP_LOGW(TAG, "Recovered state from %s", h->tmp_path);
            h->dirty = true;
        }
    }
    unlink(h->tmp_path);
    ESP_LOGI(TAG, "Loaded %u entries from %s", (unsigned)h->count, h->path);
}

/* ---------- flush ---------- */

static cJSON *build_state_json_locked(const struct cap_exec_state_t *h)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_CreateArray();

    if (!root || !array) {
        cJSON_Delete(root);
        cJSON_Delete(array);
        return NULL;
    }
    cJSON_AddNumberToObject(root, EXEC_STATE_JSON_KEY_VERSION, CAP_EXEC_STATE_FORMAT_VERSION);
    cJSON_AddItemToObject(root, EXEC_STATE_JSON_KEY_ENTRIES, array);
    for (size_t i = 0; i < h->count; i++) {
        cJSON *item = cap_exec_state_entry_to_cjson(&h->entries[i]);

        if (!item) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddItemToArray(array, item);
    }
    return root;
}

/* Serializes the state, evicting the oldest entries until it fits max_file_bytes. */
static char *serialize_with_budget_locked(struct cap_exec_state_t *h)
{
    for (;;) {
        cJSON *root = build_state_json_locked(h);
        char *text = root ? cJSON_PrintUnformatted(root) : NULL;
        size_t len;
        size_t avg;
        size_t drop;

        cJSON_Delete(root);
        if (!text) {
            return NULL;
        }
        len = strlen(text);
        if (len <= h->max_file_bytes || h->count == 0) {
            return text;
        }
        free(text);
        avg = len / h->count;
        drop = avg ? (len - h->max_file_bytes + avg - 1) / avg : 1;
        drop = drop ? drop : 1;
        ESP_LOGW(TAG, "State exceeds %u bytes, evicting %u oldest entries",
                 (unsigned)h->max_file_bytes, (unsigned)drop);
        evict_head(h, drop);
    }
}

static esp_err_t write_state_file(const struct cap_exec_state_t *h, const char *text)
{
    size_t len = strlen(text);
    FILE *fp = fopen(h->tmp_path, "wb");

    if (!fp) {
        ESP_LOGE(TAG, "Cannot open %s: errno=%d", h->tmp_path, errno);
        return ESP_FAIL;
    }
    if (fwrite(text, 1, len, fp) != len || fflush(fp) != 0) {
        ESP_LOGE(TAG, "Write to %s failed: errno=%d", h->tmp_path, errno);
        fclose(fp);
        unlink(h->tmp_path);
        return ESP_FAIL;
    }
    if (fclose(fp) != 0) {
        ESP_LOGE(TAG, "Close of %s failed: errno=%d", h->tmp_path, errno);
        unlink(h->tmp_path);
        return ESP_FAIL;
    }
    /* FAT/SPIFFS rename does not overwrite; load recovers the tmp if we crash in between. */
    unlink(h->path);
    if (rename(h->tmp_path, h->path) != 0) {
        ESP_LOGE(TAG, "Rename %s -> %s failed: errno=%d", h->tmp_path, h->path, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void arm_flush_timer(struct cap_exec_state_t *h, uint32_t delay_ms)
{
    if (!h->flush_timer || esp_timer_is_active(h->flush_timer)) {
        return;
    }
    esp_timer_start_once(h->flush_timer, (uint64_t)delay_ms * 1000ULL);
}

static esp_err_t flush_now(struct cap_exec_state_t *h)
{
    char *text = NULL;
    esp_err_t err;

    xSemaphoreTake(h->flush_lock, portMAX_DELAY);
    xSemaphoreTake(h->lock, portMAX_DELAY);
    if (!h->dirty) {
        xSemaphoreGive(h->lock);
        xSemaphoreGive(h->flush_lock);
        return ESP_OK;
    }
    text = serialize_with_budget_locked(h);
    if (text) {
        h->dirty = false;
    }
    xSemaphoreGive(h->lock);

    if (!text) {
        ESP_LOGE(TAG, "Failed to serialize state");
        err = ESP_ERR_NO_MEM;
    } else {
        err = write_state_file(h, text);
        free(text);
    }
    if (err != ESP_OK) {
        xSemaphoreTake(h->lock, portMAX_DELAY);
        h->dirty = true;
        xSemaphoreGive(h->lock);
        arm_flush_timer(h, h->flush_delay_ms > 0 ? (uint32_t)h->flush_delay_ms : EXEC_STATE_FLUSH_RETRY_MS);
    }
    xSemaphoreGive(h->flush_lock);
    return err;
}

/* Runs in the esp_timer task: only wakes the flush task, no I/O here. */
static void flush_timer_cb(void *arg)
{
    struct cap_exec_state_t *h = arg;

    if (h->flush_task) {
        xTaskNotifyGive(h->flush_task);
    }
}

static void flush_task_fn(void *arg)
{
    struct cap_exec_state_t *h = arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (h->stopping) {
            break;
        }
        flush_now(h);
    }
    xSemaphoreGive(h->task_done);
    vTaskDelete(NULL);
}

/* ---------- lifecycle ---------- */

static esp_err_t resolve_paths(struct cap_exec_state_t *h, const char *path)
{
    int n;

    if (path && path[0]) {
        n = snprintf(h->path, sizeof(h->path), "%s", path);
        if (n < 0 || (size_t)n >= sizeof(h->path)) {
            return ESP_ERR_INVALID_SIZE;
        }
    } else {
        esp_err_t err = claw_paths_join(CLAW_PATH_DATA, EXEC_STATE_DEFAULT_SUBPATH, h->path, sizeof(h->path));

        if (err != ESP_OK) {
            return err;
        }
    }
    snprintf(h->tmp_path, sizeof(h->tmp_path), "%s%s", h->path, EXEC_STATE_TMP_SUFFIX);
    snprintf(h->bad_path, sizeof(h->bad_path), "%s%s", h->path, EXEC_STATE_BAD_SUFFIX);
    return ESP_OK;
}

esp_err_t cap_exec_state_create(const cap_exec_state_config_t *config,
                                cap_exec_state_handle_t *ret_handle)
{
    struct cap_exec_state_t *h = NULL;
    esp_err_t err;

    if (!ret_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    *ret_handle = NULL;
    h = calloc(1, sizeof(*h));
    if (!h) {
        return ESP_ERR_NO_MEM;
    }
    err = resolve_paths(h, config ? config->path : NULL);
    if (err != ESP_OK) {
        free(h);
        return err;
    }
    h->max_entries = (config && config->max_entries) ? config->max_entries : CONFIG_CAP_EXEC_STATE_MAX_ENTRIES;
    h->max_file_bytes = (config && config->max_file_bytes) ? config->max_file_bytes
                        : CONFIG_CAP_EXEC_STATE_MAX_FILE_BYTES;
    h->flush_delay_ms = (config && config->flush_delay_ms >= 0) ? config->flush_delay_ms
                        : CONFIG_CAP_EXEC_STATE_FLUSH_DELAY_MS;

    h->entries = heap_caps_calloc(h->max_entries, sizeof(h->entries[0]), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!h->entries) {
        h->entries = calloc(h->max_entries, sizeof(h->entries[0]));
    }
    h->lock = xSemaphoreCreateMutex();
    h->flush_lock = xSemaphoreCreateMutex();
    h->task_done = xSemaphoreCreateBinary();
    if (!h->entries || !h->lock || !h->flush_lock || !h->task_done) {
        cap_exec_state_delete(h);
        return ESP_ERR_NO_MEM;
    }
    load_state(h);
    *ret_handle = h;
    return ESP_OK;
}

esp_err_t cap_exec_state_start(cap_exec_state_handle_t handle)
{
    const esp_timer_create_args_t timer_args = {
        .callback = flush_timer_cb,
        .arg = handle,
        .name = "exec_state_flush",
    };

    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->started) {
        return ESP_OK;
    }
    handle->stopping = false;
    if (xTaskCreate(flush_task_fn, EXEC_STATE_FLUSH_TASK_NAME, EXEC_STATE_FLUSH_TASK_STACK, handle,
                    EXEC_STATE_FLUSH_TASK_PRIO, &handle->flush_task) != pdPASS) {
        handle->flush_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (esp_timer_create(&timer_args, &handle->flush_timer) != ESP_OK) {
        handle->stopping = true;
        xTaskNotifyGive(handle->flush_task);
        xSemaphoreTake(handle->task_done, portMAX_DELAY);
        handle->flush_task = NULL;
        handle->flush_timer = NULL;
        return ESP_ERR_NO_MEM;
    }
    handle->started = true;
    if (handle->dirty) {
        arm_flush_timer(handle, handle->flush_delay_ms > 0 ? (uint32_t)handle->flush_delay_ms : 1);
    }
    return ESP_OK;
}

esp_err_t cap_exec_state_stop(cap_exec_state_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->started) {
        return ESP_OK;
    }
    handle->started = false;
    esp_timer_stop(handle->flush_timer);
    esp_timer_delete(handle->flush_timer);
    handle->flush_timer = NULL;
    handle->stopping = true;
    xTaskNotifyGive(handle->flush_task);
    xSemaphoreTake(handle->task_done, portMAX_DELAY);
    handle->flush_task = NULL;
    return flush_now(handle);
}

esp_err_t cap_exec_state_delete(cap_exec_state_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    cap_exec_state_stop(handle);
    if (handle->lock && handle->flush_lock && handle->entries) {
        flush_now(handle);
    }
    if (handle->task_done) {
        vSemaphoreDelete(handle->task_done);
    }
    if (handle->flush_lock) {
        vSemaphoreDelete(handle->flush_lock);
    }
    if (handle->lock) {
        vSemaphoreDelete(handle->lock);
    }
    free(handle->entries);
    memset(handle, 0, sizeof(*handle));
    free(handle);
    return ESP_OK;
}

esp_err_t cap_exec_state_flush(cap_exec_state_handle_t handle)
{
    return handle ? flush_now(handle) : ESP_ERR_INVALID_ARG;
}

/* ---------- operations ---------- */

esp_err_t cap_exec_state_mark_ex(cap_exec_state_handle_t handle,
                                 const char *key,
                                 const char *kind,
                                 cap_exec_state_status_t status,
                                 const char *summary,
                                 uint32_t *out_run_count)
{
    cap_exec_state_entry_t *entry = NULL;
    int64_t now;
    int index;

    if (!handle || !key_is_valid(key) || (int)status < 0 || status >= CAP_EXEC_STATE_STATUS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    now = claw_utils_time_now_ms();

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    index = find_index(handle, key);
    if (index >= 0) {
        entry = &handle->entries[index];
        entry->run_count = entry->run_count == UINT32_MAX ? UINT32_MAX : entry->run_count + 1;
        move_to_tail(handle, (size_t)index);
        entry = &handle->entries[handle->count - 1];
    } else {
        if (handle->count >= handle->max_entries) {
            evict_head(handle, 1);
        }
        entry = &handle->entries[handle->count++];
        memset(entry, 0, sizeof(*entry));
        strlcpy(entry->key, key, sizeof(entry->key));
        entry->first_seen_ms = now;
        entry->run_count = 1;
    }
    utf8_truncate_copy(entry->kind, sizeof(entry->kind), kind, CAP_EXEC_STATE_KIND_MAX_LEN);
    utf8_truncate_copy(entry->summary, sizeof(entry->summary), summary, CAP_EXEC_STATE_SUMMARY_MAX_LEN);
    entry->status = status;
    entry->last_run_ms = now;
    if (out_run_count) {
        *out_run_count = entry->run_count;
    }
    handle->dirty = true;
    xSemaphoreGive(handle->lock);

    if (handle->flush_delay_ms == 0) {
        return flush_now(handle);
    }
    arm_flush_timer(handle, (uint32_t)handle->flush_delay_ms);
    return ESP_OK;
}

esp_err_t cap_exec_state_mark(cap_exec_state_handle_t handle,
                              const char *key,
                              const char *kind,
                              cap_exec_state_status_t status,
                              const char *summary)
{
    return cap_exec_state_mark_ex(handle, key, kind, status, summary, NULL);
}

esp_err_t cap_exec_state_check(cap_exec_state_handle_t handle,
                               const char *key,
                               cap_exec_state_entry_t *out_entry)
{
    esp_err_t err = ESP_ERR_NOT_FOUND;
    int index;

    if (!handle || !key_is_valid(key)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    index = find_index(handle, key);
    if (index >= 0) {
        if (out_entry) {
            *out_entry = handle->entries[index];
        }
        err = ESP_OK;
    }
    xSemaphoreGive(handle->lock);
    return err;
}

size_t cap_exec_state_count(cap_exec_state_handle_t handle)
{
    size_t count;

    if (!handle) {
        return 0;
    }
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    count = handle->count;
    xSemaphoreGive(handle->lock);
    return count;
}

static bool render_list(const cap_exec_state_entry_t *newest_first,
                        size_t n,
                        bool truncated,
                        char *buf,
                        size_t buf_size)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_CreateArray();
    bool ok = false;

    if (!root || !array) {
        cJSON_Delete(array);
        cJSON_Delete(root);
        return false;
    }
    cJSON_AddNumberToObject(root, "count", (double)n);
    cJSON_AddItemToObject(root, "entries", array);
    for (size_t i = 0; i < n; i++) {
        cJSON *item = cap_exec_state_entry_to_cjson(&newest_first[i]);

        if (!item) {
            goto done;
        }
        cJSON_AddItemToArray(array, item);
    }
    if (truncated) {
        cJSON_AddTrueToObject(root, "truncated");
    }
    ok = cJSON_PrintPreallocated(root, buf, (int)buf_size, false);
done:
    cJSON_Delete(root);
    return ok;
}

esp_err_t cap_exec_state_list_json(cap_exec_state_handle_t handle,
                                   size_t limit,
                                   char *buf,
                                   size_t buf_size)
{
    cap_exec_state_entry_t *copy = NULL;
    size_t k;
    size_t lo;
    size_t hi;
    int best = -1;

    if (!handle || !buf || buf_size == 0 || buf_size > INT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    k = (limit == 0 || limit > handle->count) ? handle->count : limit;
    if (k) {
        copy = heap_caps_malloc(k * sizeof(*copy), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!copy) {
            copy = malloc(k * sizeof(*copy));
        }
        if (!copy) {
            xSemaphoreGive(handle->lock);
            return ESP_ERR_NO_MEM;
        }
        for (size_t i = 0; i < k; i++) {
            copy[i] = handle->entries[handle->count - 1 - i];
        }
    }
    xSemaphoreGive(handle->lock);

    if (render_list(copy, k, false, buf, buf_size)) {
        free(copy);
        return ESP_OK;
    }
    /* Rendered size grows monotonically with n: binary search the largest prefix that fits. */
    lo = 0;
    hi = k ? k - 1 : 0;
    while (k && lo <= hi) {
        size_t mid = lo + (hi - lo) / 2;

        if (render_list(copy, mid, true, buf, buf_size)) {
            best = (int)mid;
            lo = mid + 1;
        } else if (mid == 0) {
            break;
        } else {
            hi = mid - 1;
        }
    }
    if (best < 0 || !render_list(copy, (size_t)best, true, buf, buf_size)) {
        free(copy);
        buf[0] = '\0';
        return ESP_ERR_NO_MEM;
    }
    free(copy);
    return ESP_OK;
}
