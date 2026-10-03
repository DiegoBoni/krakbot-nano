/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_activity_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cap_activity_log_priv.h"
#include "claw_paths.h"
#include "claw_task.h"
#include "esp_log.h"

static const char *TAG = "cap_activity_log";

#define ACTIVITY_APPEND_LOCK_TIMEOUT_MS 5
#define ACTIVITY_STOP_WAIT_MS           5000
#define ACTIVITY_TASK_NAME              "activity_log_flush"
#define ACTIVITY_TASK_PRIORITY          2
#define ACTIVITY_DEFAULT_SUBDIR         "activity"
#define ACTIVITY_ACTIVE_NAME            "activity.jsonl"
#define ACTIVITY_OLD_NAME               "activity.1.jsonl"

static const char *const s_origin_names[CAP_ACTIVITY_ORIGIN_MAX] = {
    [CAP_ACTIVITY_ORIGIN_PROACTIVE_PULSE] = "proactive_pulse",
    [CAP_ACTIVITY_ORIGIN_SCHEDULED_TASK] = "scheduled_task",
    [CAP_ACTIVITY_ORIGIN_MANUAL] = "manual",
};

static const char *const s_result_names[CAP_ACTIVITY_RESULT_MAX] = {
    [CAP_ACTIVITY_RESULT_COMPLETED] = "completed",
    [CAP_ACTIVITY_RESULT_SKIPPED] = "skipped",
    [CAP_ACTIVITY_RESULT_ERROR] = "error",
};

static struct cap_activity_log_t *s_log;

struct cap_activity_log_t *activity_log_instance(void)
{
    struct cap_activity_log_t *log = s_log;

    return (log && log->running) ? log : NULL;
}

const char *cap_activity_origin_to_str(cap_activity_origin_t origin)
{
    return ((unsigned)origin < CAP_ACTIVITY_ORIGIN_MAX) ? s_origin_names[origin] : NULL;
}

const char *cap_activity_result_to_str(cap_activity_result_t result)
{
    return ((unsigned)result < CAP_ACTIVITY_RESULT_MAX) ? s_result_names[result] : NULL;
}

esp_err_t cap_activity_origin_from_str(const char *str, cap_activity_origin_t *out)
{
    if (!str || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < CAP_ACTIVITY_ORIGIN_MAX; i++) {
        if (strcmp(str, s_origin_names[i]) == 0) {
            *out = (cap_activity_origin_t)i;
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t cap_activity_result_from_str(const char *str, cap_activity_result_t *out)
{
    if (!str || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < CAP_ACTIVITY_RESULT_MAX; i++) {
        if (strcmp(str, s_result_names[i]) == 0) {
            *out = (cap_activity_result_t)i;
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t activity_resolve_paths(struct cap_activity_log_t *log, const char *base_dir)
{
    int n;

    if (base_dir && base_dir[0]) {
        n = snprintf(log->dir, sizeof(log->dir), "%s", base_dir);
        if (n < 0 || (size_t)n >= sizeof(log->dir)) {
            return ESP_ERR_INVALID_SIZE;
        }
    } else {
        esp_err_t err = claw_paths_join(CLAW_PATH_DATA, ACTIVITY_DEFAULT_SUBDIR, log->dir, sizeof(log->dir));

        if (err != ESP_OK) {
            return err;
        }
    }
    n = snprintf(log->path_active, sizeof(log->path_active), "%s/%s", log->dir, ACTIVITY_ACTIVE_NAME);
    if (n < 0 || (size_t)n >= sizeof(log->path_active)) {
        return ESP_ERR_INVALID_SIZE;
    }
    n = snprintf(log->path_old, sizeof(log->path_old), "%s/%s", log->dir, ACTIVITY_OLD_NAME);
    if (n < 0 || (size_t)n >= sizeof(log->path_old)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static void activity_free(struct cap_activity_log_t *log)
{
    if (!log) {
        return;
    }
    if (log->buf_mutex) {
        vSemaphoreDelete(log->buf_mutex);
    }
    if (log->file_mutex) {
        vSemaphoreDelete(log->file_mutex);
    }
    if (log->flush_mutex) {
        vSemaphoreDelete(log->flush_mutex);
    }
    if (log->done_sem) {
        vSemaphoreDelete(log->done_sem);
    }
    free(log->ring);
    free(log->batch);
    free(log->line_buf);
    free(log);
}

esp_err_t cap_activity_log_create(const cap_activity_log_config_t *config,
                                  cap_activity_log_handle_t *ret_handle)
{
    struct cap_activity_log_t *log;
    esp_err_t err;

    if (!ret_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_log) {
        return ESP_ERR_INVALID_STATE;
    }

    log = calloc(1, sizeof(*log));
    if (!log) {
        return ESP_ERR_NO_MEM;
    }

    log->cap = CONFIG_CAP_ACTIVITY_LOG_BUFFER_ENTRIES;
    log->flush_batch = CONFIG_CAP_ACTIVITY_LOG_FLUSH_BATCH;
    if (log->flush_batch > log->cap) {
        ESP_LOGW(TAG, "FLUSH_BATCH (%u) > BUFFER_ENTRIES (%u); clamping batch",
                 (unsigned)log->flush_batch, (unsigned)log->cap);
        log->flush_batch = log->cap;
    }
    log->max_bytes = (config && config->max_bytes) ? config->max_bytes : CONFIG_CAP_ACTIVITY_LOG_MAX_BYTES;

    /* Plain malloc: with SPIRAM_USE_MALLOC large blocks already land in PSRAM. */
    log->ring = calloc(log->cap, sizeof(activity_entry_t));
    log->batch = calloc(log->flush_batch, sizeof(activity_entry_t));
    log->line_buf = malloc(ACTIVITY_LINE_MAX);
    log->buf_mutex = xSemaphoreCreateMutex();
    log->file_mutex = xSemaphoreCreateMutex();
    log->flush_mutex = xSemaphoreCreateMutex();
    log->done_sem = xSemaphoreCreateBinary();
    if (!log->ring || !log->batch || !log->line_buf || !log->buf_mutex || !log->file_mutex ||
            !log->flush_mutex || !log->done_sem) {
        activity_free(log);
        return ESP_ERR_NO_MEM;
    }

    err = activity_resolve_paths(log, config ? config->base_dir : NULL);
    if (err != ESP_OK) {
        activity_free(log);
        return err;
    }

    s_log = log;
    *ret_handle = log;
    return ESP_OK;
}

static void activity_flush_task(void *arg)
{
    struct cap_activity_log_t *log = arg;

    while (log->running) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CONFIG_CAP_ACTIVITY_LOG_FLUSH_INTERVAL_MS));
        activity_store_flush(log);
    }
    xSemaphoreGive(log->done_sem);
    claw_task_delete(NULL);
}

esp_err_t cap_activity_log_start(cap_activity_log_handle_t handle)
{
    claw_task_config_t task_config = {
        .name = ACTIVITY_TASK_NAME,
        .stack_size = CONFIG_CAP_ACTIVITY_LOG_TASK_STACK,
        .priority = ACTIVITY_TASK_PRIORITY,
        .core_id = tskNO_AFFINITY,
        .stack_policy = CLAW_TASK_STACK_INTERNAL_ONLY,
    };
    esp_err_t err;

    if (!handle || handle != s_log) {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->running) {
        return ESP_ERR_INVALID_STATE;
    }

    err = activity_store_recover(handle);
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(handle->done_sem, 0);
    handle->running = true;
    if (claw_task_create(&task_config, activity_flush_task, handle, &handle->flush_task) != pdPASS) {
        handle->running = false;
        handle->flush_task = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t cap_activity_log_stop(cap_activity_log_handle_t handle)
{
    if (!handle || handle != s_log) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->running) {
        return ESP_OK;
    }

    handle->running = false;
    xTaskNotifyGive(handle->flush_task);
    if (xSemaphoreTake(handle->done_sem, pdMS_TO_TICKS(ACTIVITY_STOP_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "flush task did not exit in time");
    }
    handle->flush_task = NULL;
    /* flush_mutex serialises this with any flusher that is still finishing. */
    activity_store_flush(handle);
    return ESP_OK;
}

esp_err_t cap_activity_log_delete(cap_activity_log_handle_t handle)
{
    if (!handle || handle != s_log) {
        return ESP_ERR_INVALID_ARG;
    }
    cap_activity_log_stop(handle);
    s_log = NULL;
    activity_free(handle);
    return ESP_OK;
}

esp_err_t cap_activity_log_append(cap_activity_origin_t origin,
                                  const char *action,
                                  const char *reason,
                                  cap_activity_result_t result,
                                  const char *ref,
                                  const char *detail)
{
    struct cap_activity_log_t *log = activity_log_instance();
    activity_entry_t entry;
    bool need_flush;

    if (!log) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((unsigned)origin >= CAP_ACTIVITY_ORIGIN_MAX || (unsigned)result >= CAP_ACTIVITY_RESULT_MAX ||
            !action || !action[0] || !reason || !reason[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Built outside the lock to keep the critical section to a copy. */
    entry.ts_ms = activity_now_ms();
    entry.origin = origin;
    entry.result = result;
    activity_utf8_safe_copy(entry.action, CAP_ACTIVITY_LOG_ACTION_MAX, action);
    activity_utf8_safe_copy(entry.reason, CAP_ACTIVITY_LOG_REASON_MAX, reason);
    activity_utf8_safe_copy(entry.ref, CAP_ACTIVITY_LOG_REF_MAX, ref ? ref : "");
    activity_utf8_safe_copy(entry.detail, CAP_ACTIVITY_LOG_DETAIL_MAX, detail ? detail : "");

    /* buf_mutex is a leaf lock: nothing external is called while it is held. */
    if (xSemaphoreTake(log->buf_mutex, pdMS_TO_TICKS(ACTIVITY_APPEND_LOCK_TIMEOUT_MS)) != pdTRUE) {
        /* Racy counter bump without the lock is acceptable for a diagnostic stat. */
        log->stats.dropped++;
        return ESP_ERR_TIMEOUT;
    }
    if (log->count == log->cap) {
        log->head = (log->head + 1) % log->cap;
        log->count--;
        log->stats.dropped++;
    }
    log->ring[(log->head + log->count) % log->cap] = entry;
    log->count++;
    need_flush = log->count >= log->flush_batch;
#if CONFIG_CAP_ACTIVITY_LOG_FLUSH_ON_ERROR
    need_flush = need_flush || result == CAP_ACTIVITY_RESULT_ERROR;
#endif
    xSemaphoreGive(log->buf_mutex);

    if (need_flush && log->flush_task) {
        xTaskNotifyGive(log->flush_task);
    }
    return ESP_OK;
}

esp_err_t cap_activity_log_get_stats(cap_activity_log_stats_t *out)
{
    struct cap_activity_log_t *log = s_log;

    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!log) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(log->buf_mutex, portMAX_DELAY);
    *out = log->stats;
    out->pending = log->count;
    xSemaphoreGive(log->buf_mutex);
    return ESP_OK;
}
