/* SPDX-License-Identifier: Apache-2.0 */
#include "cap_proactive.h"
#include <math.h>
#include <stdio.h>
#include "claw_cap.h"
#include "cJSON.h"

#define CAP_PROACTIVE_GROUP_ID "cap_proactive"
#define CAP_PROACTIVE_UNREGISTER_TIMEOUT_MS 5000
/* Registry has context-free callbacks. This is an adapter reference, not
 * runtime state; app owns lifetime, all runtime state belongs to the object. */
static cap_proactive_handle_t s_registered;

static esp_err_t write_status(esp_err_t err, char *output, size_t size,
                              const char *rule, const int32_t *requested,
                              uint32_t applied, bool clamped)
{
    if (!output || !size) {
        return ESP_ERR_INVALID_ARG;
    }
    if (err != ESP_OK) {
        snprintf(output, size, "{\"ok\":false,\"error\":\"%s\",\"code\":\"%s\"}",
                 err == ESP_ERR_INVALID_STATE ? "scheduler/router no disponible o item incompatible" :
                 err == ESP_ERR_INVALID_ARG ? "seconds es obligatorio y debe ser entero positivo int32" : "proactive operation failed",
                 esp_err_to_name(err));
        return err;
    }
    cap_proactive_status_t status;
    err = cap_proactive_get_status(s_registered, &status);
    if (err != ESP_OK) {
        return write_status(err, output, size, NULL, NULL, 0, false);
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    bool ok = cJSON_AddBoolToObject(root, "ok", true) &&
              cJSON_AddBoolToObject(root, "enabled", status.enabled) &&
              cJSON_AddNumberToObject(root, "interval_sec", status.interval_sec) &&
              cJSON_AddNumberToObject(root, "min_interval_sec", status.min_interval_sec) &&
              cJSON_AddNumberToObject(root, "max_interval_sec", status.max_interval_sec) &&
              cJSON_AddBoolToObject(root, "item_present", status.item_present) &&
              cJSON_AddBoolToObject(root, "rule_present", status.rule_present) &&
              cJSON_AddBoolToObject(root, "has_next_run", status.has_next_run) &&
              cJSON_AddStringToObject(root, "interval_source", status.item_present ? "configured" : "default, no configurado");
    if (status.has_next_run) {
        ok = ok && cJSON_AddNumberToObject(root, "next_run", (double)status.next_run);
    }
    if (rule) {
        ok = ok && cJSON_AddStringToObject(root, "rule", rule);
    }
    if (requested) {
        ok = ok && cJSON_AddNumberToObject(root, "requested", *requested) &&
             cJSON_AddNumberToObject(root, "applied", applied) &&
             cJSON_AddBoolToObject(root, "clamped", clamped);
    }
    if (!status.enabled && !requested && !rule) {
        ok = ok && cJSON_AddStringToObject(root, "message", "ya está OFF");
    }
    err = !ok ? ESP_ERR_NO_MEM :
          cJSON_PrintPreallocated(root, output, size, false) ? ESP_OK : ESP_ERR_INVALID_SIZE;
    cJSON_Delete(root);
    return err;
}

static esp_err_t enable_execute(const char *input, const claw_cap_call_context_t *ctx, char *output, size_t size)
{
    (void)input;
    (void)ctx;
    if (!output || !size) {
        return ESP_ERR_INVALID_ARG;
    }
    cap_proactive_rule_result_t rule = CAP_PROACTIVE_RULE_EXISTING;
    esp_err_t err = cap_proactive_enable(s_registered, &rule);
    return write_status(err, output, size,
                         rule == CAP_PROACTIVE_RULE_CREATED ? "created" :
                         rule == CAP_PROACTIVE_RULE_UPDATED ? "updated" : "existing", NULL, 0, false);
}

static esp_err_t disable_execute(const char *input, const claw_cap_call_context_t *ctx, char *output, size_t size)
{
    (void)input;
    (void)ctx;
    if (!output || !size) {
        return ESP_ERR_INVALID_ARG;
    }
    return write_status(cap_proactive_disable(s_registered), output, size, NULL, NULL, 0, false);
}

static esp_err_t status_execute(const char *input, const claw_cap_call_context_t *ctx, char *output, size_t size)
{
    (void)input;
    (void)ctx;
    return write_status(ESP_OK, output, size, NULL, NULL, 0, false);
}

static esp_err_t interval_execute(const char *input, const claw_cap_call_context_t *ctx, char *output, size_t size)
{
    (void)ctx;
    if (!output || !size) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = input ? cJSON_Parse(input) : NULL;
    const cJSON *seconds = cJSON_GetObjectItemCaseSensitive(root, "seconds");
    /* cJSON numbers are doubles: validate BEFORE narrowing, including NaN,
     * fractional values and overflow. No valueint saturation or string coercion. */
    bool valid = cJSON_IsObject(root) && cJSON_IsNumber(seconds) &&
                 isfinite(seconds->valuedouble) && seconds->valuedouble >= INT32_MIN &&
                 seconds->valuedouble <= INT32_MAX && trunc(seconds->valuedouble) == seconds->valuedouble;
    int32_t requested = valid ? (int32_t)seconds->valuedouble : 0;
    cJSON_Delete(root);
    if (!valid) {
        return write_status(ESP_ERR_INVALID_ARG, output, size, NULL, NULL, 0, false);
    }
    uint32_t applied = 0;
    bool clamped = false;
    esp_err_t err = cap_proactive_set_interval(s_registered, requested, &applied, &clamped);
    return write_status(err, output, size, NULL, &requested, applied, clamped);
}

#define DESCRIPTOR(name_, description_, schema_, execute_) { \
    .id = name_, .name = name_, .family = "proactive", \
    .description = description_, .kind = CLAW_CAP_KIND_CALLABLE, \
    .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM, .input_schema_json = schema_, .execute = execute_ \
}
#define EMPTY_SCHEMA "{\"type\":\"object\",\"properties\":{}}"
static const claw_cap_descriptor_t s_descriptors[] = {
    DESCRIPTOR("proactive_enable", "Enable autonomous pulses, repairing the router rule.", EMPTY_SCHEMA, enable_execute),
    DESCRIPTOR("proactive_disable", "Disable future pulses without deleting configuration.", EMPTY_SCHEMA, disable_execute),
    DESCRIPTOR("proactive_status", "Read actual pulse configuration; next_run is Unix milliseconds.", EMPTY_SCHEMA, status_execute),
    DESCRIPTOR("proactive_set_interval", "Set seconds, clamping to configured limits and preserving ON/OFF.",
               "{\"type\":\"object\",\"properties\":{\"seconds\":{\"type\":\"integer\"}},\"required\":[\"seconds\"]}", interval_execute),
};
static const claw_cap_group_t s_group = {
    .group_id = CAP_PROACTIVE_GROUP_ID,
    .descriptors = s_descriptors,
    .descriptor_count = sizeof(s_descriptors) / sizeof(s_descriptors[0]),
};

esp_err_t cap_proactive_register_group(cap_proactive_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_registered) {
        return s_registered == handle ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (claw_cap_group_exists(CAP_PROACTIVE_GROUP_ID)) {
        return ESP_ERR_INVALID_STATE;
    }
    s_registered = handle;
    esp_err_t err = claw_cap_register_group(&s_group);
    if (err != ESP_OK) {
        s_registered = NULL;
    }
    return err;
}

esp_err_t cap_proactive_unregister_group(cap_proactive_handle_t handle)
{
    if (s_registered != handle) {
        return ESP_OK;
    }
    esp_err_t err = claw_cap_unregister_group(CAP_PROACTIVE_GROUP_ID, CAP_PROACTIVE_UNREGISTER_TIMEOUT_MS);
    if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
        s_registered = NULL;
        return ESP_OK;
    }
    return err;
}
