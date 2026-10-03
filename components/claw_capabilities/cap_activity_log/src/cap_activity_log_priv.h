/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cap_activity_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* Worst case: every byte of every string escapes to a 6-byte \u00XX, plus
 * keys, enum strings, a 20-digit ts and the trailing newline. */
#define ACTIVITY_LINE_OVERHEAD 160
#define ACTIVITY_LINE_MAX (ACTIVITY_LINE_OVERHEAD + 6 * (CAP_ACTIVITY_LOG_ACTION_MAX + \
                           CAP_ACTIVITY_LOG_REASON_MAX + CAP_ACTIVITY_LOG_REF_MAX + \
                           CAP_ACTIVITY_LOG_DETAIL_MAX) + 2)
#define ACTIVITY_PATH_MAX 192

/* An empty ref/detail means "absent". */
typedef struct {
    int64_t ts_ms;
    cap_activity_origin_t origin;
    cap_activity_result_t result;
    char action[CAP_ACTIVITY_LOG_ACTION_MAX + 1];
    char reason[CAP_ACTIVITY_LOG_REASON_MAX + 1];
    char ref[CAP_ACTIVITY_LOG_REF_MAX + 1];
    char detail[CAP_ACTIVITY_LOG_DETAIL_MAX + 1];
} activity_entry_t;

/*
 * Lock order: flush_mutex (outermost, serialises flushers) is never held by
 * append/list. buf_mutex and file_mutex are leaf locks and are NEVER held at
 * the same time; nothing external is called while holding either.
 */
struct cap_activity_log_t {
    SemaphoreHandle_t buf_mutex;    /* ring, stats counters */
    SemaphoreHandle_t file_mutex;   /* files, active_size, need_leading_nl */
    SemaphoreHandle_t flush_mutex;  /* batch/line scratch buffers */
    SemaphoreHandle_t done_sem;     /* flush task exit signal */

    activity_entry_t *ring;
    uint32_t cap;
    uint32_t head;
    uint32_t count;

    activity_entry_t *batch;        /* flush_batch entries, owned by flush_mutex */
    char *line_buf;                 /* ACTIVITY_LINE_MAX, owned by flush_mutex */
    uint32_t flush_batch;

    TaskHandle_t flush_task;
    volatile bool running;

    char dir[ACTIVITY_PATH_MAX];
    char path_active[ACTIVITY_PATH_MAX];
    char path_old[ACTIVITY_PATH_MAX];
    size_t active_size;
    uint32_t max_bytes;
    bool need_leading_nl;
    cap_activity_log_stats_t stats; /* pending is computed on read */
};

/* Text helpers */
void activity_utf8_safe_copy(char *dst, size_t max, const char *src);
/* Appends the escaped src at dst[pos]; returns the new pos or (size_t)-1 if it does not fit. */
size_t activity_json_escape_append(char *dst, size_t cap, size_t pos, const char *src);
/* Returns the line length (including '\n'), or 0 if it does not fit. */
size_t activity_serialize_line(const activity_entry_t *entry, char *buf, size_t cap);
int64_t activity_now_ms(void);

/* Store */
void activity_store_flush(struct cap_activity_log_t *log);
esp_err_t activity_store_recover(struct cap_activity_log_t *log);

/* Returns the singleton only while it is running, else NULL. */
struct cap_activity_log_t *activity_log_instance(void);
