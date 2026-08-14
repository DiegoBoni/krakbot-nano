/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_llm_inspect.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_core_llm.h"

/* Approximate public list pricing in USD per 1M tokens (input, output).
 * Matched by lowercase substring against the configured model name, in
 * order, so more specific entries must precede their generic fallbacks.
 * Prices drift over time; treat the estimate as directional, not exact. */
typedef struct {
    const char *needle;
    double input_usd_per_1m;
    double output_usd_per_1m;
} cap_cost_price_entry_t;

static const cap_cost_price_entry_t s_cost_price_table[] = {
    { "gpt-4o-mini", 0.15, 0.60 },
    { "gpt-4o", 2.50, 10.00 },
    { "gpt-4.1-mini", 0.40, 1.60 },
    { "gpt-4.1", 2.00, 8.00 },
    { "gpt-3.5", 0.50, 1.50 },
    { "claude-3-5-haiku", 0.80, 4.00 },
    { "claude-3.5-haiku", 0.80, 4.00 },
    { "claude-opus", 15.00, 75.00 },
    { "claude", 3.00, 15.00 },
    { "llama-3.3-70b", 0.59, 0.79 },
    { "llama-3.1-70b", 0.59, 0.79 },
    { "llama-3.1-8b", 0.05, 0.08 },
    { "llama", 0.20, 0.20 },
    { "mixtral", 0.24, 0.24 },
    { "gemini-1.5-flash", 0.075, 0.30 },
    { "gemini-2.0-flash", 0.10, 0.40 },
    { "gemini-1.5-pro", 1.25, 5.00 },
};

static bool cap_cost_find_price(const char *model, double *out_input, double *out_output)
{
    char lower[64];
    size_t i;

    if (!model || !model[0]) {
        return false;
    }
    for (i = 0; model[i] && i + 1 < sizeof(lower); i++) {
        lower[i] = (char)tolower((unsigned char)model[i]);
    }
    lower[i] = '\0';

    for (i = 0; i < sizeof(s_cost_price_table) / sizeof(s_cost_price_table[0]); i++) {
        if (strstr(lower, s_cost_price_table[i].needle)) {
            *out_input = s_cost_price_table[i].input_usd_per_1m;
            *out_output = s_cost_price_table[i].output_usd_per_1m;
            return true;
        }
    }
    return false;
}

static esp_err_t cap_cost_usage_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output,
                                        size_t output_size)
{
    claw_core_llm_usage_totals_t totals = {0};
    double price_input = 0.0;
    double price_output = 0.0;
    bool has_price;

    (void)input_json;
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ctx || !ctx->core) {
        snprintf(output, output_size, "Error: claw_core is not ready");
        return ESP_ERR_INVALID_STATE;
    }

    claw_core_llm_get_usage_totals(ctx->core, &totals);

    if (totals.request_count == 0) {
        snprintf(output, output_size, "No LLM requests recorded yet since boot.");
        return ESP_OK;
    }

    has_price = cap_cost_find_price(totals.model, &price_input, &price_output);

    if (has_price) {
        double cost_usd = (double)totals.prompt_tokens / 1000000.0 * price_input +
                          (double)totals.completion_tokens / 1000000.0 * price_output;
        snprintf(output, output_size,
                "Token usage since boot (model: %s)\n"
                "Requests: %" PRIu32 "\n"
                "Prompt tokens: %" PRIu64 "\n"
                "Completion tokens: %" PRIu64 "\n"
                "Total tokens: %" PRIu64 "\n"
                "Estimated cost: ~$%.4f USD (approx, based on public list pricing)",
                totals.model[0] ? totals.model : "unknown",
                totals.request_count,
                totals.prompt_tokens,
                totals.completion_tokens,
                totals.prompt_tokens + totals.completion_tokens,
                cost_usd);
    } else {
        snprintf(output, output_size,
                "Token usage since boot (model: %s)\n"
                "Requests: %" PRIu32 "\n"
                "Prompt tokens: %" PRIu64 "\n"
                "Completion tokens: %" PRIu64 "\n"
                "Total tokens: %" PRIu64 "\n"
                "Estimated cost: unavailable (no pricing data for this model)",
                totals.model[0] ? totals.model : "unknown",
                totals.request_count,
                totals.prompt_tokens,
                totals.completion_tokens,
                totals.prompt_tokens + totals.completion_tokens);
    }
    return ESP_OK;
}

static const char *CAP_LLM_INSPECT_SYSTEM_PROMPT =
    "You analyze local image files for the ESP32 claw. "
    "Describe visible content plainly and briefly. "
    "If the image is unclear, say what is uncertain instead of guessing.";

static esp_err_t cap_llm_inspect_execute(const char *input_json,
                                         const claw_cap_call_context_t *ctx,
                                         char *output,
                                         size_t output_size)
{
    claw_media_asset_t asset = {0};
    claw_llm_media_request_t request = {0};
    cJSON *root = NULL;
    cJSON *path_json = NULL;
    cJSON *prompt_json = NULL;
    char *analysis = NULL;
    char *error_message = NULL;
    esp_err_t err;

    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ctx || !ctx->core) {
        snprintf(output, output_size, "Error: claw_core is not ready");
        return ESP_ERR_INVALID_STATE;
    }

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: input must be a JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    path_json = cJSON_GetObjectItem(root, "path");
    prompt_json = cJSON_GetObjectItem(root, "prompt");
    if (!cJSON_IsString(path_json) || !path_json->valuestring[0] ||
            !cJSON_IsString(prompt_json) || !prompt_json->valuestring[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: path and prompt are required");
        return ESP_ERR_INVALID_ARG;
    }

    asset.kind = CLAW_MEDIA_ASSET_KIND_LOCAL_PATH;
    asset.path = path_json->valuestring;
    request.system_prompt = CAP_LLM_INSPECT_SYSTEM_PROMPT;
    request.user_prompt = prompt_json->valuestring;
    request.media = &asset;
    request.media_count = 1;
    err = claw_core_llm_infer_media(ctx->core, &request, &analysis, &error_message);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(output,
                 output_size,
                 "Error: image analysis failed (%s)%s%s",
                 esp_err_to_name(err),
                 error_message ? ": " : "",
                 error_message ? error_message : "");
        free(error_message);
        return err;
    }

    snprintf(output, output_size, "%s", analysis ? analysis : "");
    free(analysis);
    free(error_message);
    return ESP_OK;
}

static const claw_cap_descriptor_t s_llm_inspect_descriptors[] = {
    {
        .id = "inspect_image",
        .name = "inspect_image",
        .family = "system",
        .description =
        "Analyze a local image from an absolute path. Confirm the path first, then provide a prompt describing what to inspect.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"prompt\":{\"type\":\"string\"}},\"required\":[\"path\",\"prompt\"]}",
        .execute = cap_llm_inspect_execute,
    },
    {
        .id = "cost_usage",
        .name = "cost_usage",
        .family = "system",
        .description =
        "Report cumulative LLM token usage since boot and an approximate USD cost estimate for the configured model.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_cost_usage_execute,
    },
};

static const claw_cap_group_t s_llm_inspect_group = {
    .group_id = "cap_llm_inspect",
    .descriptors = s_llm_inspect_descriptors,
    .descriptor_count = sizeof(s_llm_inspect_descriptors) / sizeof(s_llm_inspect_descriptors[0]),
};

esp_err_t cap_llm_inspect_register_group(void)
{
    if (claw_cap_group_exists(s_llm_inspect_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_llm_inspect_group);
}
