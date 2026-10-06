/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_scheduler_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "cap_activity_log.h"
#include "claw_cap.h"
#include "esp_log.h"

/*
 * Terminal outcomes for scheduled fires. The scheduler publishes events with correlation_id
 * "<task id>:<planned_ms>"; the router forwards it in the cap call context. When the router-executed
 * send_message / run_script cap returns, this observer appends the real result. It keeps no state, so a
 * fire that already started still logs after its task is removed. wake_agent and async scripts finish
 * inside claw_core / a background task and are not observable here; they are not logged as completed.
 */

#define SCHEDULER_ROUTER_SOURCE   "claw_event_router"
#define SCHEDULER_CAP_SEND_PREFIX "send_message"
#define SCHEDULER_CAP_RUN_SCRIPT  "lua_run_script"
#define SCHEDULER_ID_MAX          64

static const char *TAG = "cap_scheduler_act";

static bool scheduler_correlation_task_id(const char *correlation, char *id, size_t id_size)
{
    const char *colon;
    const char *p;
    size_t len;

    if (!correlation || (colon = strrchr(correlation, ':')) == NULL || colon == correlation ||
            colon[1] == '\0') {
        return false;
    }
    for (p = colon + 1; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }
    len = (size_t)(colon - correlation);
    if (len >= id_size) {
        return false;
    }
    memcpy(id, correlation, len);
    id[len] = '\0';
    return true;
}

static bool scheduler_cap_is_terminal_action(const char *cap_name)
{
    if (!cap_name) {
        return false;
    }
    /* lua_run_script_async only reports that the script started, so it is deliberately excluded. */
    return strstr(cap_name, SCHEDULER_CAP_SEND_PREFIX) != NULL || strcmp(cap_name, SCHEDULER_CAP_RUN_SCRIPT) == 0;
}

static void scheduler_call_observer(const char *cap_name,
                                    const claw_cap_call_context_t *ctx,
                                    esp_err_t err,
                                    const char *output,
                                    void *user_ctx)
{
    char task_id[SCHEDULER_ID_MAX];
    char reason[CAP_ACTIVITY_LOG_REASON_MAX + 1];
    char detail[CAP_ACTIVITY_LOG_DETAIL_MAX + 1];
    esp_err_t log_err;

    (void)user_ctx;
    if (!ctx || !ctx->source_cap || strcmp(ctx->source_cap, SCHEDULER_ROUTER_SOURCE) != 0 ||
            !scheduler_cap_is_terminal_action(cap_name) ||
            !scheduler_correlation_task_id(ctx->correlation_id, task_id, sizeof(task_id))) {
        return;
    }
    snprintf(reason, sizeof(reason), "schedule %s %s", task_id, cap_name);
    snprintf(detail, sizeof(detail), "correlation=%s %s: %s", ctx->correlation_id,
             err == ESP_OK ? "ok" : esp_err_to_name(err),
             (output && output[0]) ? output : "-");
    log_err = cap_activity_log_append(CAP_ACTIVITY_ORIGIN_SCHEDULED_TASK,
                                      "scheduled task finished",
                                      reason,
                                      err == ESP_OK ? CAP_ACTIVITY_RESULT_COMPLETED : CAP_ACTIVITY_RESULT_ERROR,
                                      task_id,
                                      detail);
    if (log_err != ESP_OK && log_err != ESP_ERR_INVALID_STATE) {
        /* Log failures never affect the scheduler or other tasks. */
        ESP_LOGD(TAG, "activity log append failed: %s", esp_err_to_name(log_err));
    }
}

void cap_scheduler_activity_install(void)
{
    claw_cap_set_call_observer(scheduler_call_observer, NULL);
}

void cap_scheduler_activity_uninstall(void)
{
    claw_cap_set_call_observer(NULL, NULL);
}
