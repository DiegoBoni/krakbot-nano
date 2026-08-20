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
#include <strings.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_core_llm.h"
#include "claw_event_publisher.h"
#include "esp_log.h"

static const char *TAG = "cap_llm_inspect";

/* Cap on how much of a plain-text document is pulled into a conversation
 * turn - generous for a chat message, small enough for the device's memory. */
#define CAP_LLM_INSPECT_TEXT_MAX_BYTES (64 * 1024)

static const char *const s_text_document_extensions[] = {
    ".txt", ".md", ".json", ".jsonl", ".jsonc", ".csv", ".log", ".lua",
};

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
    "You analyze a local image or PDF document for the ESP32 claw. "
    "Describe or summarize its content plainly and briefly. "
    "If it is unclear or unreadable, say what is uncertain instead of guessing.";

#define CAP_LLM_INSPECT_DOCUMENT_PROMPT \
    "Summarize this PDF document concisely: its main topic, and anything worth remembering " \
    "about the user or context (a preference, a fact, an important date or detail)."

static esp_err_t cap_llm_inspect_run_media_prompt(claw_core_handle_t core,
                                                   const char *path,
                                                   const char *prompt,
                                                   char **out_analysis,
                                                   char **out_error_message)
{
    claw_media_asset_t asset = {0};
    claw_llm_media_request_t request = {0};

    asset.kind = CLAW_MEDIA_ASSET_KIND_LOCAL_PATH;
    asset.path = path;
    request.system_prompt = CAP_LLM_INSPECT_SYSTEM_PROMPT;
    request.user_prompt = prompt;
    request.media = &asset;
    request.media_count = 1;
    return claw_core_llm_infer_media(core, &request, out_analysis, out_error_message);
}

static bool cap_llm_inspect_has_extension(const char *path, const char *ext)
{
    size_t path_len = path ? strlen(path) : 0;
    size_t ext_len = strlen(ext);

    if (path_len < ext_len) {
        return false;
    }
    return strcasecmp(path + path_len - ext_len, ext) == 0;
}

static bool cap_llm_inspect_is_pdf(const char *path)
{
    return cap_llm_inspect_has_extension(path, ".pdf");
}

static bool cap_llm_inspect_is_text_document(const char *path)
{
    size_t i;

    for (i = 0; i < sizeof(s_text_document_extensions) / sizeof(s_text_document_extensions[0]); i++) {
        if (cap_llm_inspect_has_extension(path, s_text_document_extensions[i])) {
            return true;
        }
    }
    return false;
}

// Bounded whole-file read: null-terminated so it can be used as chat text
// directly. Rejects anything over max_bytes rather than truncating, since a
// silently-cut document would misrepresent what the agent "read".
static esp_err_t cap_llm_inspect_read_text_file(const char *path, size_t max_bytes, char **out_text)
{
    struct stat st = {0};
    FILE *file = NULL;
    char *buf = NULL;
    size_t read_len;

    *out_text = NULL;
    if (stat(path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (st.st_size <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if ((size_t)st.st_size > max_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }

    file = fopen(path, "rb");
    if (!file) {
        return ESP_FAIL;
    }
    buf = calloc(1, (size_t)st.st_size + 1);
    if (!buf) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    read_len = fread(buf, 1, (size_t)st.st_size, file);
    fclose(file);
    if (read_len != (size_t)st.st_size) {
        free(buf);
        return ESP_FAIL;
    }

    *out_text = buf;
    return ESP_OK;
}

static esp_err_t cap_llm_inspect_execute(const char *input_json,
                                         const claw_cap_call_context_t *ctx,
                                         char *output,
                                         size_t output_size)
{
    cJSON *root = NULL;
    cJSON *path_json = NULL;
    cJSON *prompt_json = NULL;
    cJSON *chat_id_json = NULL;
    cJSON *channel_json = NULL;
    cJSON *sender_id_json = NULL;
    cJSON *message_id_json = NULL;
    cJSON *delete_after_json = NULL;
    const char *path = NULL;
    const char *chat_id = NULL;
    const char *channel = NULL;
    const char *sender_id = NULL;
    const char *message_id = NULL;
    bool delete_after = false;
    char *analysis = NULL;
    char *error_message = NULL;
    esp_err_t err;

    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
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
    path = path_json->valuestring;

    /* Only present when a router rule routes a saved IM attachment here
     * (mirrors cap_groq_transcribe's saved_path/chat_id/channel contract):
     * lets this call also republish the analysis as an inbound chat message
     * and clean up the source file, without changing plain ad-hoc use by
     * the agent (inspecting an arbitrary local path stays output-only). */
    chat_id_json = cJSON_GetObjectItem(root, "chat_id");
    channel_json = cJSON_GetObjectItem(root, "channel");
    sender_id_json = cJSON_GetObjectItem(root, "sender_id");
    message_id_json = cJSON_GetObjectItem(root, "message_id");
    delete_after_json = cJSON_GetObjectItem(root, "delete_after");
    if (cJSON_IsString(chat_id_json) && chat_id_json->valuestring[0]) {
        chat_id = chat_id_json->valuestring;
    }
    if (cJSON_IsString(channel_json) && channel_json->valuestring[0]) {
        channel = channel_json->valuestring;
    }
    if (cJSON_IsString(sender_id_json) && sender_id_json->valuestring[0]) {
        sender_id = sender_id_json->valuestring;
    }
    if (cJSON_IsString(message_id_json) && message_id_json->valuestring[0]) {
        message_id = message_id_json->valuestring;
    }
    delete_after = cJSON_IsTrue(delete_after_json);

    /* Checked here (after path/delete_after are already parsed) rather than
     * up front, so a missing core still deletes the source file below instead
     * of leaking it - this exact gap once left every router-triggered image/
     * PDF attachment stuck on the DATA partition forever. */
    if (!ctx || !ctx->core) {
        if (delete_after) {
            remove(path);
        }
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: claw_core is not ready");
        return ESP_ERR_INVALID_STATE;
    }

    err = cap_llm_inspect_run_media_prompt(ctx->core, path, prompt_json->valuestring,
                                           &analysis, &error_message);

    if (delete_after) {
        remove(path);
    }

    if (err != ESP_OK) {
        snprintf(output,
                 output_size,
                 "Error: image analysis failed (%s)%s%s",
                 esp_err_to_name(err),
                 error_message ? ": " : "",
                 error_message ? error_message : "");
        cJSON_Delete(root);
        free(error_message);
        return err;
    }

    if (channel && chat_id) {
        esp_err_t pub_err = claw_event_router_publish_message("cap_llm_inspect",
                                                               channel,
                                                               chat_id,
                                                               analysis ? analysis : "",
                                                               sender_id,
                                                               message_id);
        if (pub_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to republish image analysis as message: %s", esp_err_to_name(pub_err));
        }
    }

    cJSON_Delete(root);
    snprintf(output, output_size, "%s", analysis ? analysis : "");
    free(analysis);
    free(error_message);
    return ESP_OK;
}

// Handles inbound "document" IM attachments (anything that isn't audio or an
// image): PDFs go through the same vision-capable LLM pipeline as images,
// small text-like files are read directly (no LLM call needed), and anything
// else gets an honest "can't read this" reply instead of vanishing silently.
// Always meant to be router-triggered (unlike inspect_image, no ad-hoc/output
// -only mode) - chat_id/channel are expected, and delete_after defaults to
// deleting since the whole point is processing the attachment once and
// freeing the DATA partition.
static esp_err_t cap_llm_inspect_document_execute(const char *input_json,
                                                   const claw_cap_call_context_t *ctx,
                                                   char *output,
                                                   size_t output_size)
{
    cJSON *root = NULL;
    cJSON *path_json = NULL;
    cJSON *chat_id_json = NULL;
    cJSON *channel_json = NULL;
    cJSON *sender_id_json = NULL;
    cJSON *message_id_json = NULL;
    cJSON *delete_after_json = NULL;
    cJSON *filename_json = NULL;
    const char *path = NULL;
    const char *chat_id = NULL;
    const char *channel = NULL;
    const char *sender_id = NULL;
    const char *message_id = NULL;
    const char *filename = NULL;
    bool delete_after = true;
    char *message_text = NULL;
    char status[96];

    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: input must be a JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    path_json = cJSON_GetObjectItem(root, "path");
    if (!cJSON_IsString(path_json) || !path_json->valuestring[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: path is required");
        return ESP_ERR_INVALID_ARG;
    }
    path = path_json->valuestring;

    chat_id_json = cJSON_GetObjectItem(root, "chat_id");
    channel_json = cJSON_GetObjectItem(root, "channel");
    sender_id_json = cJSON_GetObjectItem(root, "sender_id");
    message_id_json = cJSON_GetObjectItem(root, "message_id");
    delete_after_json = cJSON_GetObjectItem(root, "delete_after");
    filename_json = cJSON_GetObjectItem(root, "original_filename");
    if (cJSON_IsString(chat_id_json) && chat_id_json->valuestring[0]) {
        chat_id = chat_id_json->valuestring;
    }
    if (cJSON_IsString(channel_json) && channel_json->valuestring[0]) {
        channel = channel_json->valuestring;
    }
    if (cJSON_IsString(sender_id_json) && sender_id_json->valuestring[0]) {
        sender_id = sender_id_json->valuestring;
    }
    if (cJSON_IsString(message_id_json) && message_id_json->valuestring[0]) {
        message_id = message_id_json->valuestring;
    }
    if (cJSON_IsString(filename_json) && filename_json->valuestring[0]) {
        filename = filename_json->valuestring;
    }
    if (cJSON_IsBool(delete_after_json)) {
        delete_after = cJSON_IsTrue(delete_after_json);
    }
    if (!filename) {
        filename = path;
    }

    if (cap_llm_inspect_is_pdf(path)) {
        char *analysis = NULL;
        char *error_message = NULL;
        esp_err_t err = ESP_ERR_INVALID_STATE;

        if (ctx && ctx->core) {
            err = cap_llm_inspect_run_media_prompt(ctx->core, path, CAP_LLM_INSPECT_DOCUMENT_PROMPT,
                                                   &analysis, &error_message);
        }
        if (err == ESP_OK && analysis) {
            char header[128];
            snprintf(header, sizeof(header), "[PDF: %s]\n\n", filename);
            message_text = calloc(1, strlen(header) + strlen(analysis) + 1);
            if (message_text) {
                strcpy(message_text, header);
                strcat(message_text, analysis);
            }
        } else {
            char body[192];
            snprintf(body, sizeof(body),
                    "Recibí el PDF %s pero no pude analizarlo (%s%s%s).",
                    filename, esp_err_to_name(err),
                    error_message ? ": " : "", error_message ? error_message : "");
            message_text = strdup(body);
        }
        free(analysis);
        free(error_message);
    } else if (cap_llm_inspect_is_text_document(path)) {
        char *content = NULL;
        esp_err_t err = cap_llm_inspect_read_text_file(path, CAP_LLM_INSPECT_TEXT_MAX_BYTES, &content);

        if (err == ESP_OK && content) {
            char header[128];
            snprintf(header, sizeof(header), "[Documento: %s]\n\n", filename);
            message_text = calloc(1, strlen(header) + strlen(content) + 1);
            if (message_text) {
                strcpy(message_text, header);
                strcat(message_text, content);
            }
            free(content);
        } else {
            char body[192];
            snprintf(body, sizeof(body),
                    "Recibí el documento %s pero no pude leerlo (%s).",
                    filename, esp_err_to_name(err));
            message_text = strdup(body);
        }
    } else {
        char body[192];
        snprintf(body, sizeof(body),
                "Recibí el archivo %s pero todavía no puedo leer este tipo de documento.",
                filename);
        message_text = strdup(body);
    }

    if (delete_after) {
        remove(path);
    }

    if (channel && chat_id) {
        esp_err_t pub_err = claw_event_router_publish_message("cap_llm_inspect",
                                                               channel,
                                                               chat_id,
                                                               message_text ? message_text : "",
                                                               sender_id,
                                                               message_id);
        if (pub_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to republish document analysis as message: %s", esp_err_to_name(pub_err));
        }
    }

    snprintf(status, sizeof(status), "Document processed: %s", filename);
    cJSON_Delete(root);
    free(message_text);
    snprintf(output, output_size, "%s", status);
    return ESP_OK;
}

static const claw_cap_descriptor_t s_llm_inspect_descriptors[] = {
    {
        .id = "inspect_image",
        .name = "inspect_image",
        .family = "system",
        .description =
        "Analyze a local image from an absolute path. Confirm the path first, then provide a prompt describing what to inspect. "
        "If chat_id and channel are also given, the analysis is republished as an inbound chat message on that "
        "channel/chat_id (used by the router to route inbound image attachments here).",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"prompt\":{\"type\":\"string\"},"
        "\"chat_id\":{\"type\":\"string\"},\"channel\":{\"type\":\"string\"},\"sender_id\":{\"type\":\"string\"},"
        "\"message_id\":{\"type\":\"string\"},\"delete_after\":{\"type\":\"boolean\"}},"
        "\"required\":[\"path\",\"prompt\"]}",
        .execute = cap_llm_inspect_execute,
    },
    {
        .id = "inspect_document",
        .name = "inspect_document",
        .family = "system",
        .description =
        "Process a locally saved document attachment from an absolute path: PDFs are summarized via the "
        "vision-capable LLM, small text-like files (.txt/.md/.json/.csv/.log/.lua) are read directly, and "
        "anything else gets an explicit \"can't read this\" reply. Deletes the source file afterward by "
        "default. If chat_id and channel are given, the result is republished as an inbound chat message "
        "(used by the router to route inbound document attachments here).",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
        "\"chat_id\":{\"type\":\"string\"},\"channel\":{\"type\":\"string\"},\"sender_id\":{\"type\":\"string\"},"
        "\"message_id\":{\"type\":\"string\"},\"original_filename\":{\"type\":\"string\"},"
        "\"delete_after\":{\"type\":\"boolean\"}},"
        "\"required\":[\"path\"]}",
        .execute = cap_llm_inspect_document_execute,
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
