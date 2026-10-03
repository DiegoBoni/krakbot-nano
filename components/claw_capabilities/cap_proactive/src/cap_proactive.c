/* SPDX-License-Identifier: Apache-2.0 */
#include "cap_proactive.h"
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "cap_scheduler.h"
#include "claw_event_router.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define CAP_PROACTIVE_MUTEX_TIMEOUT_MS 5000
#define CAP_PROACTIVE_MS_PER_SEC 1000
#define CAP_PROACTIVE_EVENT_TYPE "schedule"
#define CAP_PROACTIVE_SOURCE_CAP "cap_scheduler"
#define CAP_PROACTIVE_ACTION_INPUT "{\"text\":\"" CAP_PROACTIVE_PULSE_PROMPT "\",\"session_policy\":\"trigger\"}"
static const char *TAG = "cap_proactive";
_Static_assert(CONFIG_CAP_PROACTIVE_DEFAULT_INTERVAL_SEC >= CONFIG_CAP_PROACTIVE_MIN_INTERVAL_SEC, "default < min");
_Static_assert(CONFIG_CAP_PROACTIVE_DEFAULT_INTERVAL_SEC <= CONFIG_CAP_PROACTIVE_MAX_INTERVAL_SEC, "default > max");
_Static_assert(CONFIG_CAP_PROACTIVE_MIN_INTERVAL_SEC <= CONFIG_CAP_PROACTIVE_MAX_INTERVAL_SEC, "min > max");

struct cap_proactive_t {
    SemaphoreHandle_t mutex;
    cap_proactive_config_t dependencies;
};

static esp_err_t lock(cap_proactive_handle_t h)
{
    if (!h) {
        return ESP_ERR_INVALID_ARG;
    }
    return xSemaphoreTake(h->mutex, pdMS_TO_TICKS(CAP_PROACTIVE_MUTEX_TIMEOUT_MS)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t available(cap_proactive_handle_t h)
{
    if (!h->dependencies.scheduler_available || !h->dependencies.router_available) {
        ESP_LOGW(TAG, "scheduler/router no disponible");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t cap_proactive_clamp_interval(int32_t requested, uint32_t *applied, bool *clamped)
{
    if (requested <= 0 || !applied || !clamped) {
        return ESP_ERR_INVALID_ARG;
    }
    *applied = requested < CONFIG_CAP_PROACTIVE_MIN_INTERVAL_SEC ? CONFIG_CAP_PROACTIVE_MIN_INTERVAL_SEC :
               requested > CONFIG_CAP_PROACTIVE_MAX_INTERVAL_SEC ? CONFIG_CAP_PROACTIVE_MAX_INTERVAL_SEC : (uint32_t)requested;
    *clamped = *applied != (uint32_t)requested;
    return ESP_OK;
}

esp_err_t cap_proactive_create(const cap_proactive_config_t *config, cap_proactive_handle_t *out)
{
    if (!config || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    cap_proactive_handle_t h = calloc(1, sizeof(*h));
    if (!h) {
        return ESP_ERR_NO_MEM;
    }
    h->dependencies = *config;
    h->mutex = xSemaphoreCreateMutex();
    if (!h->mutex) {
        free(h);
        return ESP_ERR_NO_MEM;
    }
    *out = h;
    return ESP_OK;
}

/* Owned ids must retain their event contract; reject incompatible scheduler
 * items instead of accidentally enabling arbitrary work under our id. */
static bool valid_item(const cap_scheduler_item_t *item)
{
    return item->kind == CAP_SCHEDULER_ITEM_INTERVAL &&
           item->interval_ms >= (int64_t)CONFIG_CAP_PROACTIVE_MIN_INTERVAL_SEC * CAP_PROACTIVE_MS_PER_SEC &&
           item->interval_ms <= (int64_t)CONFIG_CAP_PROACTIVE_MAX_INTERVAL_SEC * CAP_PROACTIVE_MS_PER_SEC &&
           item->interval_ms % CAP_PROACTIVE_MS_PER_SEC == 0 &&
           !strcmp(item->event_type, CAP_PROACTIVE_EVENT_TYPE) &&
           !strcmp(item->event_key, CAP_PROACTIVE_ITEM_ID);
}

static esp_err_t add_item(uint32_t interval)
{
    cap_scheduler_item_t *item = calloc(1, sizeof(*item));
    if (!item) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(item->id, CAP_PROACTIVE_ITEM_ID, sizeof(item->id));
    strlcpy(item->event_type, CAP_PROACTIVE_EVENT_TYPE, sizeof(item->event_type));
    strlcpy(item->event_key, CAP_PROACTIVE_ITEM_ID, sizeof(item->event_key));
    item->kind = CAP_SCHEDULER_ITEM_INTERVAL;
    item->interval_ms = (int64_t)interval * CAP_PROACTIVE_MS_PER_SEC;
    esp_err_t err = cap_scheduler_add(item); /* calloc means initially disabled */
    free(item);
    return err;
}

static esp_err_t ensure_rule(cap_proactive_rule_result_t *result)
{
    claw_event_router_rule_t *rule = calloc(1, sizeof(*rule));
    if (!rule) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = claw_event_router_get_rule(CAP_PROACTIVE_RULE_ID, rule);
    bool exists = err == ESP_OK;
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        free(rule);
        return err;
    }
    bool same = false;
    if (exists) {
        cJSON *actual = rule->action_count == 1 ? cJSON_Parse(rule->actions[0].input_json) : NULL;
        cJSON *expected = cJSON_Parse(CAP_PROACTIVE_ACTION_INPUT);
        if (!expected) {
            cJSON_Delete(actual);
            claw_event_router_free_rule(rule);
            free(rule);
            return ESP_ERR_NO_MEM;
        }
        same = rule->enabled && rule->consume_on_match && !rule->vars_json &&
               !strcmp(rule->match.event_type, CAP_PROACTIVE_EVENT_TYPE) &&
               !strcmp(rule->match.event_key, CAP_PROACTIVE_ITEM_ID) &&
               !strcmp(rule->match.source_cap, CAP_PROACTIVE_SOURCE_CAP) &&
               !rule->match.channel[0] && !rule->match.chat_id[0] &&
               !rule->match.content_type[0] && !rule->match.text[0] &&
               rule->match.text_match_rule == CLAW_EVENT_ROUTER_TEXT_MATCH_EXACT &&
               rule->action_count == 1 && rule->actions[0].kind == CLAW_EVENT_ROUTER_ACTION_RUN_AGENT &&
               !rule->actions[0].fail_open && !rule->actions[0].capture_output &&
               rule->actions[0].caller == CLAW_CAP_CALLER_SYSTEM && !rule->actions[0].cap[0] &&
               cJSON_Compare(actual, expected, true);
        cJSON_Delete(actual);
        cJSON_Delete(expected);
        claw_event_router_free_rule(rule);
    }
    if (same) {
        *result = CAP_PROACTIVE_RULE_EXISTING;
        free(rule);
        return ESP_OK;
    }
    memset(rule, 0, sizeof(*rule));
    rule->enabled = true;
    rule->consume_on_match = true;
    strlcpy(rule->id, CAP_PROACTIVE_RULE_ID, sizeof(rule->id));
    strlcpy(rule->match.event_type, CAP_PROACTIVE_EVENT_TYPE, sizeof(rule->match.event_type));
    strlcpy(rule->match.event_key, CAP_PROACTIVE_ITEM_ID, sizeof(rule->match.event_key));
    strlcpy(rule->match.source_cap, CAP_PROACTIVE_SOURCE_CAP, sizeof(rule->match.source_cap));
    claw_event_router_action_t action = {
        .kind = CLAW_EVENT_ROUTER_ACTION_RUN_AGENT,
        .input_json = CAP_PROACTIVE_ACTION_INPUT,
        .caller = CLAW_CAP_CALLER_SYSTEM,
    };
    rule->actions = &action;
    rule->action_count = 1;
    err = exists ? claw_event_router_update_rule(rule) : claw_event_router_add_rule(rule);
    if (err == ESP_OK) {
        *result = exists ? CAP_PROACTIVE_RULE_UPDATED : CAP_PROACTIVE_RULE_CREATED;
    }
    free(rule); /* action and input are borrowed; CRUD clones them */
    return err;
}

esp_err_t cap_proactive_enable(cap_proactive_handle_t h, cap_proactive_rule_result_t *result)
{
    if (!result) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock(h);
    if (err != ESP_OK) {
        return err;
    }
    cap_scheduler_snapshot_t *snapshot = NULL;
    bool created = false;
    err = available(h);
    if (err != ESP_OK) {
        goto cleanup;
    }
    snapshot = calloc(1, sizeof(*snapshot));
    if (!snapshot) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    err = cap_scheduler_get_snapshot(CAP_PROACTIVE_ITEM_ID, snapshot);
    if (err == ESP_ERR_NOT_FOUND) {
        err = add_item(CONFIG_CAP_PROACTIVE_DEFAULT_INTERVAL_SEC);
        created = err == ESP_OK;
    } else if (err == ESP_OK && !valid_item(&snapshot->item)) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = ensure_rule(result);
    if (err != ESP_OK && created) {
        esp_err_t rollback = cap_scheduler_remove(CAP_PROACTIVE_ITEM_ID);
        if (rollback != ESP_OK) {
            ESP_LOGW(TAG, "Item rollback failed: %s", esp_err_to_name(rollback));
        }
    }
    if (err == ESP_OK && !snapshot->item.enabled) {
        err = cap_scheduler_enable(CAP_PROACTIVE_ITEM_ID, true);
    }
cleanup:
    free(snapshot);
    xSemaphoreGive(h->mutex);
    return err;
}

esp_err_t cap_proactive_disable(cap_proactive_handle_t h)
{
    esp_err_t err = lock(h);
    if (err != ESP_OK) {
        return err;
    }
    cap_scheduler_snapshot_t *snapshot = NULL;
    err = h->dependencies.scheduler_available ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        snapshot = calloc(1, sizeof(*snapshot));
        err = snapshot ? cap_scheduler_get_snapshot(CAP_PROACTIVE_ITEM_ID, snapshot) : ESP_ERR_NO_MEM;
        if (err == ESP_ERR_NOT_FOUND) {
            err = ESP_OK;
        } else if (err == ESP_OK && snapshot->item.enabled) {
            err = cap_scheduler_enable(CAP_PROACTIVE_ITEM_ID, false);
        }
    }
    free(snapshot);
    xSemaphoreGive(h->mutex);
    return err;
}

esp_err_t cap_proactive_set_interval(cap_proactive_handle_t h, int32_t requested, uint32_t *applied, bool *clamped)
{
    esp_err_t err = cap_proactive_clamp_interval(requested, applied, clamped);
    if (err != ESP_OK) {
        return err;
    }
    err = lock(h);
    if (err != ESP_OK) {
        return err;
    }
    cap_scheduler_snapshot_t *snapshot = NULL;
    err = available(h);
    if (err != ESP_OK) {
        goto cleanup;
    }
    snapshot = calloc(1, sizeof(*snapshot));
    if (!snapshot) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    err = cap_scheduler_get_snapshot(CAP_PROACTIVE_ITEM_ID, snapshot);
    if (err == ESP_ERR_NOT_FOUND) {
        err = add_item(*applied);
    } else if (err == ESP_OK) {
        if (!valid_item(&snapshot->item)) {
            err = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }
        if (snapshot->item.interval_ms != (int64_t)*applied * CAP_PROACTIVE_MS_PER_SEC) {
            snapshot->item.interval_ms = (int64_t)*applied * CAP_PROACTIVE_MS_PER_SEC;
            err = cap_scheduler_update(&snapshot->item);
        }
    }
cleanup:
    free(snapshot);
    xSemaphoreGive(h->mutex);
    return err;
}

esp_err_t cap_proactive_get_status(cap_proactive_handle_t h, cap_proactive_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock(h);
    if (err != ESP_OK) {
        return err;
    }
    memset(status, 0, sizeof(*status));
    status->interval_sec = CONFIG_CAP_PROACTIVE_DEFAULT_INTERVAL_SEC;
    status->min_interval_sec = CONFIG_CAP_PROACTIVE_MIN_INTERVAL_SEC;
    status->max_interval_sec = CONFIG_CAP_PROACTIVE_MAX_INTERVAL_SEC;
    cap_scheduler_snapshot_t *snapshot = NULL;
    claw_event_router_rule_t *rule = NULL;
    err = available(h);
    if (err != ESP_OK) {
        goto cleanup;
    }
    snapshot = calloc(1, sizeof(*snapshot));
    rule = calloc(1, sizeof(*rule));
    if (!snapshot || !rule) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    err = cap_scheduler_get_snapshot(CAP_PROACTIVE_ITEM_ID, snapshot);
    if (err == ESP_OK) {
        status->item_present = true;
        status->enabled = snapshot->item.enabled;
        status->interval_sec = snapshot->item.interval_ms / CAP_PROACTIVE_MS_PER_SEC;
        status->has_next_run = status->enabled && snapshot->next_fire_ms > 0;
        status->next_run = snapshot->next_fire_ms;
    } else if (err != ESP_ERR_NOT_FOUND) {
        goto cleanup;
    }
    err = claw_event_router_get_rule(CAP_PROACTIVE_RULE_ID, rule);
    if (err == ESP_OK) {
        status->rule_present = true;
        claw_event_router_free_rule(rule);
    } else if (err == ESP_ERR_NOT_FOUND) {
        err = ESP_OK;
    }
cleanup:
    free(rule);
    free(snapshot);
    xSemaphoreGive(h->mutex);
    return err;
}

esp_err_t cap_proactive_start(cap_proactive_handle_t h)
{
    esp_err_t err = lock(h);
    if (err != ESP_OK) {
        return err;
    }
    cap_scheduler_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    err = available(h);
    if (err == ESP_OK) {
        err = snapshot ? cap_scheduler_get_snapshot(CAP_PROACTIVE_ITEM_ID, snapshot) : ESP_ERR_NO_MEM;
        if (err == ESP_OK && snapshot->item.enabled) {
            cap_proactive_rule_result_t result;
            err = valid_item(&snapshot->item) ? ensure_rule(&result) : ESP_ERR_INVALID_STATE;
        }
    }
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "Startup failed; disabling pulse: %s", esp_err_to_name(err));
        if (h->dependencies.scheduler_available) {
            esp_err_t disable_err = cap_scheduler_enable(CAP_PROACTIVE_ITEM_ID, false);
            if (disable_err != ESP_OK && disable_err != ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "Cannot disable pulse: %s", esp_err_to_name(disable_err));
                free(snapshot);
                xSemaphoreGive(h->mutex);
                return disable_err; /* Never start scheduler after failed fail-closed recovery. */
            }
        }
    }
    free(snapshot);
    xSemaphoreGive(h->mutex);
    return ESP_OK;
}

esp_err_t cap_proactive_stop(cap_proactive_handle_t h)
{
    return cap_proactive_disable(h);
}

esp_err_t cap_proactive_delete(cap_proactive_handle_t h)
{
    if (!h) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Drain registry calls before taking the object lock: active callbacks
     * may be waiting on it. The owner has already quiesced direct API users. */
    esp_err_t err = cap_proactive_unregister_group(h);
    if (err != ESP_OK) {
        return err;
    }
    err = lock(h);
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreGive(h->mutex);
    vSemaphoreDelete(h->mutex);
    free(h);
    return ESP_OK;
}
