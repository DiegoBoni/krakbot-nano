/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAP_PROACTIVE_ITEM_ID "proactive_pulse"
#define CAP_PROACTIVE_RULE_ID "proactive_pulse_rule"
#define CAP_PROACTIVE_SKILL_NAME "proactive_pulse"
#define CAP_PROACTIVE_PULSE_PROMPT "Autonomous pulse: run skill " CAP_PROACTIVE_SKILL_NAME "."

typedef struct cap_proactive_t *cap_proactive_handle_t;
/* Dependencies use global APIs. They must be initialized before start/operations.
 * The flags allow app configuration to explicitly withhold either dependency. */
typedef struct {
    bool scheduler_available;
    bool router_available;
} cap_proactive_config_t;

typedef struct {
    bool enabled;
    uint32_t interval_sec;
    uint32_t min_interval_sec;
    uint32_t max_interval_sec;
    bool has_next_run;
    int64_t next_run; /* Unix milliseconds, as scheduler.next_fire_ms. */
    bool rule_present;
    bool item_present;
} cap_proactive_status_t;

typedef enum {
    CAP_PROACTIVE_RULE_EXISTING,
    CAP_PROACTIVE_RULE_CREATED,
    CAP_PROACTIVE_RULE_UPDATED,
} cap_proactive_rule_result_t;

esp_err_t cap_proactive_create(const cap_proactive_config_t *config, cap_proactive_handle_t *out);
esp_err_t cap_proactive_start(cap_proactive_handle_t handle);
/* stop suspends pulses by disabling the item; delete preserves persisted state.
 * Caller owns lifetime and must quiesce callers/unregister the group before delete. */
esp_err_t cap_proactive_stop(cap_proactive_handle_t handle);
esp_err_t cap_proactive_delete(cap_proactive_handle_t handle);
esp_err_t cap_proactive_enable(cap_proactive_handle_t handle, cap_proactive_rule_result_t *rule_result);
esp_err_t cap_proactive_disable(cap_proactive_handle_t handle);
esp_err_t cap_proactive_set_interval(cap_proactive_handle_t handle, int32_t requested,
                                      uint32_t *applied, bool *clamped);
esp_err_t cap_proactive_get_status(cap_proactive_handle_t handle, cap_proactive_status_t *status);
/* Pure helper; clamped is an explicit output. */
esp_err_t cap_proactive_clamp_interval(int32_t requested, uint32_t *applied, bool *clamped);
/* Registry has no per-call object context: only one registered instance is allowed. */
esp_err_t cap_proactive_register_group(cap_proactive_handle_t handle);
esp_err_t cap_proactive_unregister_group(cap_proactive_handle_t handle);

#ifdef __cplusplus
}
#endif
