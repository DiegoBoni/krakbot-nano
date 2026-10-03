/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "cap_activity_log.h"
#include "claw_cap.h"

#define ACTIVITY_LIST_SUFFIX_RESERVE 96

static esp_err_t activity_fail(char *output, size_t output_size, esp_err_t err, const char *msg)
{
    snprintf(output, output_size, "Error: %s", msg);
    return err;
}

static const char *activity_get_str(const cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);

    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : NULL;
}

static esp_err_t activity_log_list_execute(const char *input_json,
                                           const claw_cap_call_context_t *ctx,
                                           char *output,
                                           size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    cap_activity_origin_t origin = CAP_ACTIVITY_ORIGIN_MAX;
    cap_activity_log_stats_t stats = {0};
    const cJSON *item;
    const char *origin_str;
    size_t limit = 0;
    int64_t since_ms = 0;
    size_t count = 0;
    size_t prefix;
    size_t json_len;
    esp_err_t err;

    (void)ctx;
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return activity_fail(output, output_size, ESP_ERR_INVALID_ARG, "input must be a JSON object");
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "limit");
    if (cJSON_IsNumber(item) && item->valuedouble > 0) {
        limit = (size_t)item->valuedouble;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "since_ms");
    if (cJSON_IsNumber(item) && item->valuedouble > 0) {
        since_ms = (int64_t)item->valuedouble;
    }
    origin_str = activity_get_str(root, "origin");
    if (origin_str && cap_activity_origin_from_str(origin_str, &origin) != ESP_OK) {
        snprintf(output, output_size,
                 "Error: invalid origin: %s; expected proactive_pulse|scheduled_task|manual", origin_str);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(root);

    if (output_size <= sizeof("{\"entries\":") + ACTIVITY_LIST_SUFFIX_RESERVE) {
        return ESP_ERR_INVALID_SIZE;
    }
    prefix = snprintf(output, output_size, "{\"entries\":");
    err = cap_activity_log_list_json(limit, origin, since_ms, output + prefix,
                                     output_size - prefix - ACTIVITY_LIST_SUFFIX_RESERVE, &count);
    if (err == ESP_ERR_INVALID_STATE) {
        return activity_fail(output, output_size, err, "activity log not available");
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_SIZE) {
        return activity_fail(output, output_size, err, "could not read activity log");
    }
    json_len = strlen(output + prefix);
    cap_activity_log_get_stats(&stats);
    snprintf(output + prefix + json_len, output_size - prefix - json_len,
             ",\"count\":%u,\"truncated\":%s,\"dropped\":%" PRIu32 "}",
             (unsigned)count, err == ESP_ERR_INVALID_SIZE ? "true" : "false", stats.dropped);
    return ESP_OK;
}

static esp_err_t activity_log_append_execute(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output,
                                             size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    cap_activity_origin_t origin;
    cap_activity_result_t result;
    const char *action;
    const char *reason;
    esp_err_t err;

    (void)ctx;
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return activity_fail(output, output_size, ESP_ERR_INVALID_ARG, "input must be a JSON object");
    }
    action = activity_get_str(root, "action");
    reason = activity_get_str(root, "reason");

    if (cap_activity_origin_from_str(activity_get_str(root, "origin"), &origin) != ESP_OK) {
        err = activity_fail(output, output_size, ESP_ERR_INVALID_ARG,
                            "invalid origin; expected proactive_pulse|scheduled_task|manual");
    } else if (cap_activity_result_from_str(activity_get_str(root, "result"), &result) != ESP_OK) {
        err = activity_fail(output, output_size, ESP_ERR_INVALID_ARG,
                            "invalid result; expected completed|skipped|error");
    } else if (!action || !action[0]) {
        err = activity_fail(output, output_size, ESP_ERR_INVALID_ARG, "action is required");
    } else if (!reason || !reason[0]) {
        err = activity_fail(output, output_size, ESP_ERR_INVALID_ARG, "reason is required");
    } else {
        err = cap_activity_log_append(origin, action, reason, result,
                                      activity_get_str(root, "ref"), activity_get_str(root, "detail"));
        switch (err) {
        case ESP_OK:
            snprintf(output, output_size,
                     "{\"ok\":true,\"note\":\"entry is pending flush to flash but already visible in activity_log_list\"}");
            break;
        case ESP_ERR_INVALID_STATE:
            activity_fail(output, output_size, err, "activity log not available");
            break;
        case ESP_ERR_TIMEOUT:
            activity_fail(output, output_size, err, "activity log busy, retry");
            break;
        default:
            activity_fail(output, output_size, err, "could not append entry");
            break;
        }
    }
    cJSON_Delete(root);
    return err;
}

static const claw_cap_descriptor_t s_activity_log_descriptors[] = {
    {
        .id = "activity_log_list",
        .name = "activity_log_list",
        .family = "system",
        .description = "List recent autonomous activity (proactive pulses, scheduled tasks, manual "
                       "actions), newest first. Check it before repeating an action or when asked "
                       "what the bot did.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{"
        "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":50,\"description\":\"Max entries (default 20)\"},"
        "\"origin\":{\"type\":\"string\",\"enum\":[\"proactive_pulse\",\"scheduled_task\",\"manual\"]},"
        "\"since_ms\":{\"type\":\"integer\",\"description\":\"Only entries with ts >= since_ms (epoch ms)\"}}}",
        .execute = activity_log_list_execute,
    },
    {
        .id = "activity_log_append",
        .name = "activity_log_append",
        .family = "system",
        .description = "Record the outcome of an autonomous action in the activity log. detail is "
                       "truncated to 200 characters; never include secrets, tokens or private messages.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{"
        "\"origin\":{\"type\":\"string\",\"enum\":[\"proactive_pulse\",\"scheduled_task\",\"manual\"]},"
        "\"action\":{\"type\":\"string\",\"description\":\"What was done (short)\"},"
        "\"reason\":{\"type\":\"string\",\"description\":\"Why it was done (short)\"},"
        "\"result\":{\"type\":\"string\",\"enum\":[\"completed\",\"skipped\",\"error\"]},"
        "\"ref\":{\"type\":\"string\",\"description\":\"Related task id or file path\"},"
        "\"detail\":{\"type\":\"string\",\"description\":\"Short detail, max 200 chars, no secrets\"}},"
        "\"required\":[\"origin\",\"action\",\"reason\",\"result\"]}",
        .execute = activity_log_append_execute,
    },
};

static const claw_cap_group_t s_activity_log_group = {
    .group_id = "cap_activity_log",
    .descriptors = s_activity_log_descriptors,
    .descriptor_count = sizeof(s_activity_log_descriptors) / sizeof(s_activity_log_descriptors[0]),
};

esp_err_t cap_activity_log_register_group(void)
{
    if (claw_cap_group_exists(s_activity_log_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_activity_log_group);
}
