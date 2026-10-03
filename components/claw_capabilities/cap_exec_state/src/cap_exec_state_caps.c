/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "cap_exec_state.h"
#include "cap_exec_state_priv.h"
#include "claw_cap.h"
#include "esp_log.h"

static const char *TAG = "cap_exec_state_caps";

#define EXEC_STATE_GROUP_ID "cap_exec_state"

static cap_exec_state_handle_t s_default;

cJSON *cap_exec_state_entry_to_cjson(const cap_exec_state_entry_t *entry)
{
    cJSON *obj = cJSON_CreateObject();

    if (!obj) {
        return NULL;
    }
    if (!cJSON_AddStringToObject(obj, "key", entry->key) ||
            !cJSON_AddStringToObject(obj, "kind", entry->kind) ||
            !cJSON_AddStringToObject(obj, "status", cap_exec_state_status_to_str(entry->status)) ||
            !cJSON_AddNumberToObject(obj, "first_seen_ms", (double)entry->first_seen_ms) ||
            !cJSON_AddNumberToObject(obj, "last_run_ms", (double)entry->last_run_ms) ||
            !cJSON_AddNumberToObject(obj, "run_count", (double)entry->run_count) ||
            !cJSON_AddStringToObject(obj, "summary", entry->summary)) {
        cJSON_Delete(obj);
        return NULL;
    }
    return obj;
}

static esp_err_t write_json(cJSON *root, char *output, size_t output_size)
{
    esp_err_t err = ESP_ERR_NO_MEM;

    if (root && cJSON_PrintPreallocated(root, output, (int)output_size, false)) {
        err = ESP_OK;
    }
    cJSON_Delete(root);
    return err;
}

/* Validation errors are reported to the model as JSON without failing the tool call. */
static esp_err_t write_error(char *output, size_t output_size, const char *message)
{
    cJSON *root = cJSON_CreateObject();

    if (root) {
        cJSON_AddFalseToObject(root, "ok");
        cJSON_AddStringToObject(root, "error", message);
    }
    return write_json(root, output, output_size);
}

static const char *json_string(const cJSON *root, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);

    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static esp_err_t execute_check(const char *input_json, const claw_cap_call_context_t *ctx,
                               char *output, size_t output_size)
{
    cJSON *in = cJSON_Parse(input_json);
    cap_exec_state_entry_t entry;
    cJSON *root = NULL;
    const char *key = NULL;
    esp_err_t err;

    (void)ctx;
    key = in ? json_string(in, "key") : NULL;
    if (!key || !key[0] || strlen(key) > CAP_EXEC_STATE_KEY_MAX_LEN) {
        cJSON_Delete(in);
        return write_error(output, output_size, "key is required (1-64 bytes)");
    }
    err = cap_exec_state_check(s_default, key, &entry);
    cJSON_Delete(in);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        return write_error(output, output_size, esp_err_to_name(err));
    }
    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "found", err == ESP_OK);
    cJSON_AddBoolToObject(root, "already_processed",
                          err == ESP_OK && entry.status == CAP_EXEC_STATE_STATUS_COMPLETED);
    if (err == ESP_OK) {
        cJSON *item = cap_exec_state_entry_to_cjson(&entry);

        if (!item) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToObject(root, "entry", item);
    }
    return write_json(root, output, output_size);
}

static esp_err_t execute_mark(const char *input_json, const claw_cap_call_context_t *ctx,
                              char *output, size_t output_size)
{
    cJSON *in = cJSON_Parse(input_json);
    cap_exec_state_status_t status;
    const char *key = NULL;
    const char *status_str = NULL;
    uint32_t run_count = 0;
    cJSON *root = NULL;
    esp_err_t err;

    (void)ctx;
    if (!cJSON_IsObject(in)) {
        cJSON_Delete(in);
        return write_error(output, output_size, "input must be a JSON object");
    }
    key = json_string(in, "key");
    status_str = json_string(in, "status");
    if (!key || !key[0] || strlen(key) > CAP_EXEC_STATE_KEY_MAX_LEN) {
        cJSON_Delete(in);
        return write_error(output, output_size, "key is required (1-64 bytes)");
    }
    if (cap_exec_state_status_from_str(status_str, &status) != ESP_OK) {
        cJSON_Delete(in);
        return write_error(output, output_size, "status must be completed, failed or skipped");
    }
    err = cap_exec_state_mark_ex(s_default, key, json_string(in, "kind"), status,
                                 json_string(in, "summary"), &run_count);
    cJSON_Delete(in);
    if (err != ESP_OK) {
        return write_error(output, output_size, esp_err_to_name(err));
    }
    root = cJSON_CreateObject();
    if (root) {
        cJSON_AddTrueToObject(root, "ok");
        cJSON_AddNumberToObject(root, "run_count", (double)run_count);
    }
    return write_json(root, output, output_size);
}

static esp_err_t execute_list(const char *input_json, const claw_cap_call_context_t *ctx,
                              char *output, size_t output_size)
{
    cJSON *in = cJSON_Parse(input_json);
    const cJSON *limit_item = in ? cJSON_GetObjectItemCaseSensitive(in, "limit") : NULL;
    size_t limit = CONFIG_CAP_EXEC_STATE_LIST_DEFAULT_LIMIT;
    esp_err_t err;

    (void)ctx;
    if (cJSON_IsNumber(limit_item) && limit_item->valuedouble >= 1) {
        limit = (size_t)limit_item->valuedouble;
    }
    cJSON_Delete(in);
    err = cap_exec_state_list_json(s_default, limit, output, output_size);
    if (err != ESP_OK) {
        return write_error(output, output_size, esp_err_to_name(err));
    }
    return ESP_OK;
}

static const claw_cap_descriptor_t s_descriptors[] = {
    {
        .id = "exec_state_check",
        .name = "exec_state_check",
        .family = "exec_state",
        .description = "Check whether a resource key was already processed. Returns found, "
                       "already_processed (status completed) and the stored entry.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\","
                             "\"description\":\"Stable key like <type>:<id>, max 64 bytes\"}},"
                             "\"required\":[\"key\"]}",
        .execute = execute_check,
    },
    {
        .id = "exec_state_mark",
        .name = "exec_state_mark",
        .family = "exec_state",
        .description = "Record the outcome of processing a resource key. Returns the updated run_count.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{"
                             "\"key\":{\"type\":\"string\",\"description\":\"Stable key like <type>:<id>, max 64 bytes\"},"
                             "\"kind\":{\"type\":\"string\",\"description\":\"Resource type, max 24 bytes\"},"
                             "\"status\":{\"type\":\"string\",\"enum\":[\"completed\",\"failed\",\"skipped\"]},"
                             "\"summary\":{\"type\":\"string\",\"description\":\"Short outcome note, max 200 bytes\"}},"
                             "\"required\":[\"key\",\"kind\",\"status\"]}",
        .execute = execute_mark,
    },
    {
        .id = "exec_state_list",
        .name = "exec_state_list",
        .family = "exec_state",
        .description = "List the most recently processed entries, newest first.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"limit\":{\"type\":\"integer\","
                             "\"minimum\":1,\"description\":\"Max entries to return\"}}}",
        .execute = execute_list,
    },
};

static esp_err_t group_start(void)
{
    return cap_exec_state_start(s_default);
}

static esp_err_t group_stop(void)
{
    return cap_exec_state_stop(s_default);
}

static const claw_cap_group_t s_group = {
    .group_id = EXEC_STATE_GROUP_ID,
    .descriptors = s_descriptors,
    .descriptor_count = sizeof(s_descriptors) / sizeof(s_descriptors[0]),
    .group_start = group_start,
    .group_stop = group_stop,
};

esp_err_t cap_exec_state_register_group(void)
{
    cap_exec_state_config_t config = CAP_EXEC_STATE_CONFIG_DEFAULT();
    esp_err_t err;

    if (claw_cap_group_exists(s_group.group_id)) {
        return ESP_OK;
    }
    if (!s_default) {
        err = cap_exec_state_create(&config, &s_default);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create exec state store: %s", esp_err_to_name(err));
            return err;
        }
    }
    err = claw_cap_register_group(&s_group);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register group: %s", esp_err_to_name(err));
        return err;
    }
    /* Start now so deferred flushes work even if the group lifecycle is not driven. */
    return cap_exec_state_start(s_default);
}
