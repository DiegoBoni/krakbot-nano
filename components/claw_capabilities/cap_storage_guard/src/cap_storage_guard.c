/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_storage_guard.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_paths.h"
#include "claw_task.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cap_storage_guard";

#define CAP_STORAGE_GUARD_PATH_LEN       160
#define CAP_STORAGE_GUARD_CHANNEL_LEN    16
#define CAP_STORAGE_GUARD_CHAT_ID_LEN    64
#define CAP_STORAGE_GUARD_MIN_INTERVAL_MS (10 * 1000)
/* Usage must drop this far below the threshold before a new alert can fire,
 * so a single overflow does not spam the admin as it hovers near the line. */
#define CAP_STORAGE_GUARD_HYSTERESIS_BYTES (256 * 1024)

typedef struct {
    uint32_t threshold_bytes;
    uint32_t check_interval_ms;
    char attachment_root_dir[CAP_STORAGE_GUARD_PATH_LEN];
    char tts_cache_root[CAP_STORAGE_GUARD_PATH_LEN];
    char admin_channel[CAP_STORAGE_GUARD_CHANNEL_LEN];
    char admin_chat_id[CAP_STORAGE_GUARD_CHAT_ID_LEN];
    TaskHandle_t task_handle;
    volatile bool stop_requested;
    bool started;
    bool alarm_active;
} cap_storage_guard_state_t;

static cap_storage_guard_state_t s_guard;

static const char *cap_storage_guard_resolve_send_cap(const char *channel)
{
    if (!channel) {
        return NULL;
    }
    if (strcmp(channel, "telegram") == 0) {
        return "tg_send_message";
    }
    if (strcmp(channel, "feishu") == 0) {
        return "feishu_send_message";
    }
    if (strcmp(channel, "qq") == 0) {
        return "qq_send_message";
    }
    if (strcmp(channel, "wechat") == 0) {
        return "wechat_send_message";
    }
    if (strcmp(channel, "web") == 0) {
        return "local_send_message";
    }
    return NULL;
}

// Recurses into dir_path, always keeping the directory itself so future
// attachments/cache files still have somewhere to land. When do_delete is
// false this only tallies size (a dry-run "how much could I free" pass).
static void cap_storage_guard_walk_dir(const char *dir_path, bool do_delete,
                                       size_t *out_count, uint64_t *out_bytes)
{
    DIR *dir = opendir(dir_path);
    struct dirent *entry;

    if (!dir) {
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        char path[CAP_STORAGE_GUARD_PATH_LEN];
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name) >= (int)sizeof(path)) {
            continue;
        }
        if (stat(path, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            cap_storage_guard_walk_dir(path, do_delete, out_count, out_bytes);
        } else if (S_ISREG(st.st_mode)) {
            if (!do_delete || remove(path) == 0) {
                (*out_count)++;
                *out_bytes += (uint64_t)st.st_size;
            }
        }
    }

    closedir(dir);
}

static esp_err_t cap_storage_guard_data_usage(uint64_t *out_used, uint64_t *out_total)
{
    const char *data_path = claw_paths_get(CLAW_PATH_DATA);
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    esp_err_t err;

    if (!data_path) {
        return ESP_ERR_INVALID_STATE;
    }
    err = esp_vfs_fat_info(data_path, &total, &free_bytes);
    if (err != ESP_OK) {
        return err;
    }
    *out_used = total - free_bytes;
    *out_total = total;
    return ESP_OK;
}

static void cap_storage_guard_send_alert(uint64_t used, uint64_t total)
{
    const char *send_cap = cap_storage_guard_resolve_send_cap(s_guard.admin_channel);
    cJSON *body = NULL;
    char *body_json = NULL;
    char message[256];
    char output[64];
    claw_cap_call_context_t call_ctx = {0};

    if (!send_cap || !s_guard.admin_chat_id[0]) {
        ESP_LOGW(TAG, "Storage alarm threshold crossed (used=%" PRIu64 " total=%" PRIu64
                 ") but no admin_channel/admin_chat_id is configured to notify", used, total);
        return;
    }

    snprintf(message, sizeof(message),
            "\xe2\x9a\xa0\xef\xb8\x8f Almacenamiento al %u%% (%" PRIu64 " KB / %" PRIu64
            " KB). Puedo borrar adjuntos guardados o cach\xc3\xa9 de audio para liberar "
            "espacio si me lo pedis (ej: \"limpia el almacenamiento\").",
            (unsigned)((used * 100) / (total ? total : 1)),
            used / 1024, total / 1024);

    body = cJSON_CreateObject();
    if (!body) {
        return;
    }
    cJSON_AddStringToObject(body, "chat_id", s_guard.admin_chat_id);
    cJSON_AddStringToObject(body, "message", message);
    body_json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_json) {
        return;
    }

    call_ctx.channel = s_guard.admin_channel;
    call_ctx.chat_id = s_guard.admin_chat_id;
    call_ctx.source_cap = "cap_storage_guard";
    call_ctx.caller = CLAW_CAP_CALLER_SYSTEM;
    claw_cap_call(send_cap, body_json, &call_ctx, output, sizeof(output));
    free(body_json);
}

static void cap_storage_guard_check_once(void)
{
    uint64_t used = 0;
    uint64_t total = 0;

    if (cap_storage_guard_data_usage(&used, &total) != ESP_OK) {
        return;
    }

    if (used >= s_guard.threshold_bytes) {
        if (!s_guard.alarm_active) {
            s_guard.alarm_active = true;
            cap_storage_guard_send_alert(used, total);
        }
        return;
    }

    uint64_t rearm_below = s_guard.threshold_bytes > CAP_STORAGE_GUARD_HYSTERESIS_BYTES
                          ? s_guard.threshold_bytes - CAP_STORAGE_GUARD_HYSTERESIS_BYTES
                          : 0;
    if (used < rearm_below) {
        s_guard.alarm_active = false;
    }
}

static void cap_storage_guard_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "storage guard task started threshold=%u interval_ms=%u",
             (unsigned)s_guard.threshold_bytes, (unsigned)s_guard.check_interval_ms);
    while (!s_guard.stop_requested) {
        cap_storage_guard_check_once();
        vTaskDelay(pdMS_TO_TICKS(s_guard.check_interval_ms));
    }
    s_guard.started = false;
    s_guard.task_handle = NULL;
    claw_task_delete(NULL);
}

esp_err_t cap_storage_guard_start(const cap_storage_guard_config_t *config)
{
    claw_task_config_t task_config = {
        .name = "cap_storage_guard",
        .stack_size = 4 * 1024,
        .priority = 2,
        .core_id = tskNO_AFFINITY,
        .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
    };
    BaseType_t ok;

    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_guard.started) {
        return ESP_OK;
    }
    if (config->threshold_bytes == 0) {
        ESP_LOGI(TAG, "Storage guard disabled (threshold_bytes=0)");
        return ESP_OK;
    }

    s_guard.threshold_bytes = config->threshold_bytes;
    s_guard.check_interval_ms = config->check_interval_ms >= CAP_STORAGE_GUARD_MIN_INTERVAL_MS
                              ? config->check_interval_ms
                              : CAP_STORAGE_GUARD_MIN_INTERVAL_MS;
    strlcpy(s_guard.attachment_root_dir,
           config->attachment_root_dir ? config->attachment_root_dir : "",
           sizeof(s_guard.attachment_root_dir));
    strlcpy(s_guard.tts_cache_root,
           config->tts_cache_root ? config->tts_cache_root : "",
           sizeof(s_guard.tts_cache_root));
    s_guard.alarm_active = false;
    s_guard.stop_requested = false;

    ok = claw_task_create(&task_config, cap_storage_guard_task, NULL, &s_guard.task_handle);
    if (ok != pdPASS) {
        s_guard.task_handle = NULL;
        return ESP_FAIL;
    }
    s_guard.started = true;
    return ESP_OK;
}

esp_err_t cap_storage_guard_stop(void)
{
    if (!s_guard.started || !s_guard.task_handle) {
        return ESP_OK;
    }
    s_guard.stop_requested = true;
    while (s_guard.task_handle) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_OK;
}

esp_err_t cap_storage_guard_set_admin(const char *channel, const char *chat_id)
{
    strlcpy(s_guard.admin_channel, channel ? channel : "", sizeof(s_guard.admin_channel));
    strlcpy(s_guard.admin_chat_id, chat_id ? chat_id : "", sizeof(s_guard.admin_chat_id));
    return ESP_OK;
}

static esp_err_t cap_storage_guard_status_execute(const char *input_json,
                                                   const claw_cap_call_context_t *ctx,
                                                   char *output,
                                                   size_t output_size)
{
    uint64_t used = 0;
    uint64_t total = 0;
    size_t attachment_count = 0;
    uint64_t attachment_bytes = 0;
    size_t tts_count = 0;
    uint64_t tts_bytes = 0;
    esp_err_t err;

    (void)input_json;
    (void)ctx;

    err = cap_storage_guard_data_usage(&used, &total);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: could not read storage usage (%s)", esp_err_to_name(err));
        return err;
    }

    if (s_guard.attachment_root_dir[0]) {
        cap_storage_guard_walk_dir(s_guard.attachment_root_dir, false, &attachment_count, &attachment_bytes);
    }
    if (s_guard.tts_cache_root[0]) {
        cap_storage_guard_walk_dir(s_guard.tts_cache_root, false, &tts_count, &tts_bytes);
    }

    snprintf(output, output_size,
            "DATA partition: %" PRIu64 " KB used / %" PRIu64 " KB total (%u%%). "
            "Reclaimable: %u saved attachment(s) (%" PRIu64 " KB), "
            "%u TTS cache file(s) (%" PRIu64 " KB). "
            "Call storage_cleanup with target=\"attachments\", \"tts_cache\" or \"all\" to free space.",
            used / 1024, total / 1024, (unsigned)((used * 100) / (total ? total : 1)),
            (unsigned)attachment_count, attachment_bytes / 1024,
            (unsigned)tts_count, tts_bytes / 1024);
    return ESP_OK;
}

static esp_err_t cap_storage_guard_cleanup_execute(const char *input_json,
                                                    const claw_cap_call_context_t *ctx,
                                                    char *output,
                                                    size_t output_size)
{
    cJSON *root = NULL;
    cJSON *target_json = NULL;
    const char *target = "all";
    size_t freed_count = 0;
    uint64_t freed_bytes = 0;
    bool did_something = false;

    (void)ctx;

    root = cJSON_Parse(input_json ? input_json : "{}");
    if (root) {
        target_json = cJSON_GetObjectItem(root, "target");
        if (cJSON_IsString(target_json) && target_json->valuestring[0]) {
            target = target_json->valuestring;
        }
    }

    if ((strcmp(target, "attachments") == 0 || strcmp(target, "all") == 0) &&
            s_guard.attachment_root_dir[0]) {
        cap_storage_guard_walk_dir(s_guard.attachment_root_dir, true, &freed_count, &freed_bytes);
        did_something = true;
    }
    if ((strcmp(target, "tts_cache") == 0 || strcmp(target, "all") == 0) &&
            s_guard.tts_cache_root[0]) {
        cap_storage_guard_walk_dir(s_guard.tts_cache_root, true, &freed_count, &freed_bytes);
        did_something = true;
    }
    cJSON_Delete(root);

    if (!did_something) {
        snprintf(output, output_size,
                "Error: unknown target '%s' (expected attachments, tts_cache or all)", target);
        return ESP_ERR_INVALID_ARG;
    }

    s_guard.alarm_active = false;
    snprintf(output, output_size, "Freed %u file(s), %" PRIu64 " KB from %s",
            (unsigned)freed_count, freed_bytes / 1024, target);
    return ESP_OK;
}

static const claw_cap_descriptor_t s_storage_guard_descriptors[] = {
    {
        .id = "storage_status",
        .name = "storage_status",
        .family = "system",
        .description = "Report DATA partition storage usage, including how much space could be "
                       "reclaimed from saved IM attachments and TTS cache.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_storage_guard_status_execute,
    },
    {
        .id = "storage_cleanup",
        .name = "storage_cleanup",
        .family = "system",
        .description = "Free up space on the DATA partition by deleting reclaimable leftover "
                       "files: saved IM attachments, TTS cache audio, or both.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"target\":{\"type\":\"string\","
        "\"enum\":[\"attachments\",\"tts_cache\",\"all\"]}},\"required\":[\"target\"]}",
        .execute = cap_storage_guard_cleanup_execute,
    },
};

static const claw_cap_group_t s_storage_guard_group = {
    .group_id = "cap_storage_guard",
    .descriptors = s_storage_guard_descriptors,
    .descriptor_count = sizeof(s_storage_guard_descriptors) / sizeof(s_storage_guard_descriptors[0]),
};

esp_err_t cap_storage_guard_register_group(void)
{
    if (claw_cap_group_exists(s_storage_guard_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_storage_guard_group);
}
