/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "cJSON.h"
#include "cap_activity_log.h"
#include "cap_activity_log_priv.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "cap_activity_log";

#define ACTIVITY_DIR_MODE        0755
#define ACTIVITY_MIN_VALID_EPOCH 1577836800LL  /* 2020-01-01: below this the clock is not synced */
#define ACTIVITY_MS_PER_SEC      1000LL

/* ---------- text helpers ---------- */

void activity_utf8_safe_copy(char *dst, size_t max, const char *src)
{
    size_t n = src ? strlen(src) : 0;

    if (n > max) {
        n = max;
        /* Do not leave a split multibyte sequence: back up to a lead byte. */
        while (n > 0 && (((unsigned char)src[n]) & 0xC0) == 0x80) {
            n--;
        }
    }
    if (n) {
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

static size_t activity_put(char *dst, size_t cap, size_t pos, const char *s, size_t n)
{
    if (pos == (size_t)-1 || pos + n >= cap) {
        return (size_t)-1;
    }
    memcpy(dst + pos, s, n);
    return pos + n;
}

size_t activity_json_escape_append(char *dst, size_t cap, size_t pos, const char *src)
{
    static const char hex[] = "0123456789abcdef";

    for (; *src; src++) {
        unsigned char c = (unsigned char)*src;
        char esc[6];
        size_t n = 2;

        esc[0] = '\\';
        switch (c) {
        case '"':  esc[1] = '"';  break;
        case '\\': esc[1] = '\\'; break;
        case '\n': esc[1] = 'n';  break;
        case '\r': esc[1] = 'r';  break;
        case '\t': esc[1] = 't';  break;
        case '\b': esc[1] = 'b';  break;
        case '\f': esc[1] = 'f';  break;
        default:
            if (c < 0x20 || c == 0x7F) {
                esc[1] = 'u'; esc[2] = '0'; esc[3] = '0';
                esc[4] = hex[c >> 4]; esc[5] = hex[c & 0xF];
                n = 6;
            } else {
                esc[0] = (char)c;
                n = 1;
            }
            break;
        }
        pos = activity_put(dst, cap, pos, esc, n);
        if (pos == (size_t)-1) {
            return pos;
        }
    }
    return pos;
}

static size_t activity_put_str_field(char *buf, size_t cap, size_t pos, const char *key, const char *val)
{
    pos = activity_put(buf, cap, pos, key, strlen(key));
    pos = activity_json_escape_append(buf, cap, pos, val);
    return activity_put(buf, cap, pos, "\"", 1);
}

size_t activity_serialize_line(const activity_entry_t *entry, char *buf, size_t cap)
{
    char num[40];
    size_t pos = 0;

    snprintf(num, sizeof(num), "{\"ts\":%" PRId64 ",\"origin\":\"%s\",", entry->ts_ms,
             cap_activity_origin_to_str(entry->origin));
    pos = activity_put(buf, cap, pos, num, strlen(num));
    pos = activity_put_str_field(buf, cap, pos, "\"action\":\"", entry->action);
    pos = activity_put_str_field(buf, cap, pos, ",\"reason\":\"", entry->reason);
    snprintf(num, sizeof(num), ",\"result\":\"%s\"", cap_activity_result_to_str(entry->result));
    pos = activity_put(buf, cap, pos, num, strlen(num));
    if (entry->ref[0]) {
        pos = activity_put_str_field(buf, cap, pos, ",\"ref\":\"", entry->ref);
    }
    if (entry->detail[0]) {
        pos = activity_put_str_field(buf, cap, pos, ",\"detail\":\"", entry->detail);
    }
    pos = activity_put(buf, cap, pos, "}\n", 2);
    if (pos == (size_t)-1) {
        return 0;
    }
    buf[pos] = '\0';
    return pos;
}

int64_t activity_now_ms(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) == 0 && tv.tv_sec >= ACTIVITY_MIN_VALID_EPOCH) {
        return (int64_t)tv.tv_sec * ACTIVITY_MS_PER_SEC + tv.tv_usec / 1000;
    }
    /* Clock not synced yet: uptime in ms, so ts is monotonic but not wall time. */
    return esp_timer_get_time() / 1000;
}

/* ---------- startup recovery ---------- */

esp_err_t activity_store_recover(struct cap_activity_log_t *log)
{
    struct stat st;
    FILE *f;

    if (mkdir(log->dir, ACTIVITY_DIR_MODE) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir %s failed: errno=%d", log->dir, errno);
        return ESP_FAIL;
    }

    xSemaphoreTake(log->file_mutex, portMAX_DELAY);
    log->active_size = 0;
    log->need_leading_nl = false;
    if (stat(log->path_active, &st) == 0 && st.st_size > 0) {
        log->active_size = (size_t)st.st_size;
        f = fopen(log->path_active, "rb");
        if (f) {
            /* A power cut can leave a truncated last line; start the next write on a new line. */
            if (fseek(f, -1, SEEK_END) == 0 && fgetc(f) != '\n') {
                log->need_leading_nl = true;
            }
            fclose(f);
        }
    }
    xSemaphoreGive(log->file_mutex);
    return ESP_OK;
}

/* ---------- flush ---------- */

static void activity_rotate_locked(struct cap_activity_log_t *log, uint32_t *rotations, bool *failed)
{
    if (unlink(log->path_old) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "unlink %s failed: errno=%d", log->path_old, errno);
    }
    if (rename(log->path_active, log->path_old) != 0) {
        ESP_LOGW(TAG, "rotate failed: errno=%d (keep writing to active segment)", errno);
        *failed = true;
        return;
    }
    log->active_size = 0;
    log->need_leading_nl = false;
    (*rotations)++;
}

/* Writes one batch under file_mutex. Entries that cannot be written are lost. */
static void activity_write_batch(struct cap_activity_log_t *log, size_t n,
                                 uint32_t *rotations, uint32_t *errors)
{
    FILE *f = NULL;
    bool rotate_failed = false;

    xSemaphoreTake(log->file_mutex, portMAX_DELAY);
    for (size_t i = 0; i < n; i++) {
        size_t len = activity_serialize_line(&log->batch[i], log->line_buf, ACTIVITY_LINE_MAX);

        if (len == 0) {
            (*errors)++;
            continue;
        }
        if (log->active_size > 0 && log->active_size + len > log->max_bytes && !rotate_failed) {
            if (f) {
                fclose(f);
                f = NULL;
            }
            activity_rotate_locked(log, rotations, &rotate_failed);
        }
        if (!f) {
            f = fopen(log->path_active, "a");
            if (!f) {
                ESP_LOGW(TAG, "open %s failed: errno=%d", log->path_active, errno);
                (*errors)++;
                break;
            }
            if (log->need_leading_nl) {
                if (fputc('\n', f) != EOF) {
                    log->active_size++;
                }
                log->need_leading_nl = false;
            }
        }
        /* No fsync: a power cut may lose the tail; recovery tolerates a partial last line. */
        if (fwrite(log->line_buf, 1, len, f) != len) {
            ESP_LOGW(TAG, "write failed: errno=%d", errno);
            (*errors)++;
            log->need_leading_nl = true;
            break;
        }
        log->active_size += len;
    }
    if (f) {
        fclose(f);
    }
    xSemaphoreGive(log->file_mutex);
}

static size_t activity_pop_batch(struct cap_activity_log_t *log)
{
    size_t n;

    xSemaphoreTake(log->buf_mutex, portMAX_DELAY);
    n = log->count < log->flush_batch ? log->count : log->flush_batch;
    for (size_t i = 0; i < n; i++) {
        log->batch[i] = log->ring[(log->head + i) % log->cap];
    }
    log->head = (log->head + n) % log->cap;
    log->count -= n;
    xSemaphoreGive(log->buf_mutex);
    return n;
}

void activity_store_flush(struct cap_activity_log_t *log)
{
    size_t n;

    xSemaphoreTake(log->flush_mutex, portMAX_DELAY);
    while ((n = activity_pop_batch(log)) > 0) {
        uint32_t rotations = 0;
        uint32_t errors = 0;

        activity_write_batch(log, n, &rotations, &errors);
        if (rotations || errors) {
            xSemaphoreTake(log->buf_mutex, portMAX_DELAY);
            log->stats.rotations += rotations;
            log->stats.flush_errors += errors;
            xSemaphoreGive(log->buf_mutex);
        }
    }
    xSemaphoreGive(log->flush_mutex);
}

/* ---------- tolerant read ---------- */

typedef struct {
    activity_entry_t *items;
    size_t limit;
    size_t next;   /* next write slot */
    size_t count;
    int64_t since_ms;
    cap_activity_origin_t origin_filter;
    const char *ref_filter;  /* NULL or empty = any ref */
} activity_results_t;

static void activity_results_push(activity_results_t *res, const activity_entry_t *e)
{
    if (e->ts_ms < res->since_ms ||
            (res->origin_filter != CAP_ACTIVITY_ORIGIN_MAX && e->origin != res->origin_filter) ||
            (res->ref_filter && res->ref_filter[0] && strcmp(e->ref, res->ref_filter) != 0)) {
        return;
    }
    res->items[res->next] = *e;
    res->next = (res->next + 1) % res->limit;
    if (res->count < res->limit) {
        res->count++;
    }
}

static bool activity_copy_str(const cJSON *obj, const char *key, bool required, char *dst, size_t max)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (!item) {
        dst[0] = '\0';
        return !required;
    }
    if (!cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    activity_utf8_safe_copy(dst, max, item->valuestring);
    return true;
}

static bool activity_parse_line(const char *line, activity_entry_t *e)
{
    cJSON *root = cJSON_Parse(line);
    const cJSON *ts;
    const cJSON *origin;
    const cJSON *result;
    bool ok = false;

    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    ts = cJSON_GetObjectItemCaseSensitive(root, "ts");
    origin = cJSON_GetObjectItemCaseSensitive(root, "origin");
    result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (cJSON_IsNumber(ts) && cJSON_IsString(origin) && cJSON_IsString(result) &&
            cap_activity_origin_from_str(origin->valuestring, &e->origin) == ESP_OK &&
            cap_activity_result_from_str(result->valuestring, &e->result) == ESP_OK &&
            activity_copy_str(root, "action", true, e->action, CAP_ACTIVITY_LOG_ACTION_MAX) &&
            activity_copy_str(root, "reason", true, e->reason, CAP_ACTIVITY_LOG_REASON_MAX) &&
            activity_copy_str(root, "ref", false, e->ref, CAP_ACTIVITY_LOG_REF_MAX) &&
            activity_copy_str(root, "detail", false, e->detail, CAP_ACTIVITY_LOG_DETAIL_MAX)) {
        e->ts_ms = (int64_t)ts->valuedouble;
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

/* Reads one segment; a missing file counts as empty. Returns skipped lines. */
static size_t activity_read_segment(const char *path, char *line, activity_results_t *res)
{
    FILE *f = fopen(path, "r");
    activity_entry_t *e = &res->items[res->limit];  /* scratch slot past the ring */
    size_t skipped = 0;
    bool discarding = false;

    if (!f) {
        return 0;
    }
    while (fgets(line, ACTIVITY_LINE_MAX, f)) {
        size_t len = strlen(line);
        bool complete = len > 0 && line[len - 1] == '\n';

        if (discarding) {
            /* Tail of an over-long line: drop until its newline. */
            discarding = !complete;
            continue;
        }
        if (!complete && !feof(f)) {
            discarding = true;
            skipped++;
            continue;
        }
        if (len == 0 || (len == 1 && complete)) {
            continue;
        }
        if (activity_parse_line(line, e)) {
            activity_results_push(res, e);
        } else {
            skipped++;
        }
    }
    fclose(f);
    return skipped;
}

esp_err_t cap_activity_log_list_json(size_t limit,
                                     cap_activity_origin_t origin_filter,
                                     int64_t since_ms,
                                     char *out,
                                     size_t out_size,
                                     size_t *out_count)
{
    return cap_activity_log_list_json_ref(limit, origin_filter, NULL, since_ms, out, out_size, out_count);
}

esp_err_t cap_activity_log_list_json_ref(size_t limit,
                                         cap_activity_origin_t origin_filter,
                                         const char *ref_filter,
                                         int64_t since_ms,
                                         char *out,
                                         size_t out_size,
                                         size_t *out_count)
{
    struct cap_activity_log_t *log = activity_log_instance();
    activity_results_t res = {0};
    char *line;
    size_t skipped = 0;
    size_t pos;
    size_t written = 0;
    esp_err_t ret = ESP_OK;

    if (out_count) {
        *out_count = 0;
    }
    if (!out || out_size < 3 || (unsigned)origin_filter > CAP_ACTIVITY_ORIGIN_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!log) {
        return ESP_ERR_INVALID_STATE;
    }
    if (limit == 0) {
        limit = CAP_ACTIVITY_LOG_LIST_DEFAULT;
    } else if (limit > CAP_ACTIVITY_LOG_LIST_MAX) {
        limit = CAP_ACTIVITY_LOG_LIST_MAX;
    }

    res.limit = limit;
    res.since_ms = since_ms;
    res.origin_filter = origin_filter;
    res.ref_filter = ref_filter;
    /* One extra slot is the parse scratch entry. Temporary, <= ~30 KB. */
    res.items = malloc((limit + 1) * sizeof(activity_entry_t));
    line = malloc(ACTIVITY_LINE_MAX);
    if (!res.items || !line) {
        free(res.items);
        free(line);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(log->file_mutex, portMAX_DELAY);
    skipped += activity_read_segment(log->path_old, line, &res);
    skipped += activity_read_segment(log->path_active, line, &res);
    xSemaphoreGive(log->file_mutex);

    /* Pending entries are newer than anything on flash; taken under a separate lock. */
    xSemaphoreTake(log->buf_mutex, portMAX_DELAY);
    for (uint32_t i = 0; i < log->count; i++) {
        activity_results_push(&res, &log->ring[(log->head + i) % log->cap]);
    }
    xSemaphoreGive(log->buf_mutex);

    if (skipped) {
        ESP_LOGD(TAG, "skipped %u unreadable line(s)", (unsigned)skipped);
    }

    /* Newest first. */
    pos = activity_put(out, out_size, 0, "[", 1);
    for (size_t i = 0; i < res.count; i++) {
        size_t idx = (res.next + res.limit - 1 - i) % res.limit;
        size_t len = activity_serialize_line(&res.items[idx], line, ACTIVITY_LINE_MAX);
        size_t need;

        if (len == 0) {
            continue;
        }
        len--;  /* drop '\n' */
        need = len + (written ? 1 : 0);
        /* Keep room for the closing bracket. */
        if (pos + need + 2 > out_size) {
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (written) {
            out[pos++] = ',';
        }
        memcpy(out + pos, line, len);
        pos += len;
        written++;
    }
    out[pos++] = ']';
    out[pos] = '\0';
    if (out_count) {
        *out_count = written;
    }

    free(res.items);
    free(line);
    return ret;
}
