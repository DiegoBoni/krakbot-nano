/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_claw_rules_seed.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_paths.h"
#include "esp_log.h"

static const char *TAG = "app_claw_rules_seed";

#define APP_CLAW_SEED_RULES_SUBPATH ".recovery/router_rules/router_rules.json"
#define APP_CLAW_SEED_PATH_LEN      128
#define APP_CLAW_FALLTHROUGH_RULE   "im_any_message_agent"

/* Rules added to the seed after the first firmware releases. */
static const char *const s_seed_rule_ids[] = {
    "im_help_command",
    "im_start_command",
    "im_new_command",
    "im_status_command",
    "im_web_command",
    "im_tasks_command",
    "im_digest_command",
    "im_mcp_command",
};

static cJSON *app_claw_seed_read_json(const char *path)
{
    FILE *file = fopen(path, "rb");
    long size;
    char *buf;
    cJSON *root;

    if (!file) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    buf = calloc(1, (size_t)size + 1);
    if (!buf || fread(buf, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        free(buf);
        return NULL;
    }
    fclose(file);
    root = cJSON_Parse(buf);
    free(buf);
    return cJSON_IsArray(root) ? root : (cJSON_Delete(root), NULL);
}

static cJSON *app_claw_seed_find_rule(const cJSON *rules, const char *id, int *out_index)
{
    cJSON *rule = NULL;
    int index = 0;

    cJSON_ArrayForEach(rule, rules) {
        const cJSON *rule_id = cJSON_GetObjectItem(rule, "id");

        if (cJSON_IsString(rule_id) && strcmp(rule_id->valuestring, id) == 0) {
            if (out_index) {
                *out_index = index;
            }
            return rule;
        }
        index++;
    }
    return NULL;
}

static bool app_claw_seed_write_json(const char *path, const cJSON *root)
{
    char *rendered = cJSON_PrintUnformatted(root);
    FILE *file;
    bool ok = false;

    if (!rendered) {
        return false;
    }
    file = fopen(path, "wb");
    if (file) {
        ok = fwrite(rendered, 1, strlen(rendered), file) == strlen(rendered);
        fclose(file);
    }
    free(rendered);
    return ok;
}

void app_claw_router_rules_merge_seed(const char *rules_path)
{
    char seed_path[APP_CLAW_SEED_PATH_LEN];
    cJSON *data = NULL;
    cJSON *seed = NULL;
    int added = 0;

    if (!rules_path || !rules_path[0] ||
            claw_paths_join(CLAW_PATH_SYSTEM, APP_CLAW_SEED_RULES_SUBPATH, seed_path, sizeof(seed_path)) != ESP_OK) {
        return;
    }
    data = app_claw_seed_read_json(rules_path);
    seed = app_claw_seed_read_json(seed_path);
    if (!data || !seed) {
        goto cleanup;
    }

    for (size_t i = 0; i < sizeof(s_seed_rule_ids) / sizeof(s_seed_rule_ids[0]); i++) {
        cJSON *rule = app_claw_seed_find_rule(seed, s_seed_rule_ids[i], NULL);
        int fallthrough_index = 0;
        int insert_at = 0;

        if (!rule || app_claw_seed_find_rule(data, s_seed_rule_ids[i], NULL)) {
            continue;
        }
        /* Command rules must run before the catch-all rule that forwards text to the agent. */
        insert_at = app_claw_seed_find_rule(data, APP_CLAW_FALLTHROUGH_RULE, &fallthrough_index)
                    ? fallthrough_index : cJSON_GetArraySize(data);
        cJSON_InsertItemInArray(data, insert_at, cJSON_Duplicate(rule, true));
        added++;
    }

    if (added > 0 && app_claw_seed_write_json(rules_path, data)) {
        ESP_LOGI(TAG, "Added %d chat command rule(s) from the recovery seed", added);
    } else if (added > 0) {
        ESP_LOGW(TAG, "Could not write merged router rules");
    }

cleanup:
    cJSON_Delete(data);
    cJSON_Delete(seed);
}
