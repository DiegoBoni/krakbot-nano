/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_openai_tts.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_utils_string.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cap_openai_tts";

static const char *const s_tts_waiting_messages[] = {
    "🔊 Afinando la voz...",
    "🎙️ Preparando el micrófono...",
    "🎵 Poniendo música a las palabras...",
    "🗣️ Practicando la pronunciación...",
    "🎧 Grabando la respuesta...",
    "🐙 Calentando las cuerdas vocales...",
};

#define CAP_TTS_OPENAI_API_URL       "https://api.openai.com/v1/audio/speech"
#define CAP_TTS_OPENAI_DEFAULT_MODEL "tts-1"
#define CAP_TTS_OPENAI_DEFAULT_VOICE "alloy"

#define CAP_TTS_GROQ_API_URL         "https://api.groq.com/openai/v1/audio/speech"
#define CAP_TTS_GROQ_DEFAULT_MODEL   "canopylabs/orpheus-v1-english"
#define CAP_TTS_GROQ_DEFAULT_VOICE   "autumn"

#define CAP_TTS_HTTP_TIMEOUT_MS 30000
#define CAP_TTS_TRANSIENT_RETRIES  1
#define CAP_TTS_RETRY_BACKOFF_MS   400
#define CAP_TTS_API_KEY_LEN   256
/* Groq's Orpheus model only outputs uncompressed WAV (OpenAI outputs
 * compact Opus), so for the same speech duration a Groq reply is roughly
 * 5-10x heavier to download from Groq and re-upload to the chat channel.
 * Capping the synthesized text bounds worst-case latency for long replies;
 * Groq gets a tighter cap since it pays that size penalty per character. */
#define CAP_TTS_MAX_TEXT_LEN_GROQ   400
#define CAP_TTS_MAX_TEXT_LEN_OPENAI 800
/* Groq's wav output defaults to 48kHz; voice replies don't need hi-fi audio,
 * and a lower sample rate shrinks that uncompressed file substantially. */
#define CAP_TTS_GROQ_SAMPLE_RATE    16000
/* Texts longer than the per-provider cap are split into several voice
 * messages instead of being cut off. The chunk count is bounded so a runaway
 * text cannot keep the device synthesizing for minutes. */
#define CAP_TTS_MAX_CHUNKS          10
/* A chunk boundary is searched only in the tail of the window so chunks stay
 * reasonably full instead of ending at the first early full stop. */
#define CAP_TTS_CHUNK_MIN_FILL_PCT  50
#define CAP_TTS_VOICE_LEN     32
#define CAP_TTS_MODEL_LEN     64
#define CAP_TTS_DIR_LEN       160
#define CAP_TTS_PATH_LEN      200
#define CAP_TTS_PROVIDER_LEN  8

typedef struct {
    FILE *file;
    size_t bytes_written;
    bool error;
} cap_openai_tts_download_t;

typedef struct {
    char api_key[CAP_TTS_API_KEY_LEN];
    char voice[CAP_TTS_VOICE_LEN];
    char model[CAP_TTS_MODEL_LEN];
    char cache_dir[CAP_TTS_DIR_LEN];
    char provider[CAP_TTS_PROVIDER_LEN];
} cap_openai_tts_state_t;

static EXT_RAM_BSS_ATTR cap_openai_tts_state_t s_tts;
static bool s_tts_initialized;

static bool cap_tts_is_groq(void)
{
    return strcmp(s_tts.provider, "groq") == 0;
}

static void cap_openai_tts_init_defaults(void)
{
    if (s_tts_initialized) {
        return;
    }
    strlcpy(s_tts.voice, CAP_TTS_OPENAI_DEFAULT_VOICE, sizeof(s_tts.voice));
    strlcpy(s_tts.model, CAP_TTS_OPENAI_DEFAULT_MODEL, sizeof(s_tts.model));
    strlcpy(s_tts.provider, "openai", sizeof(s_tts.provider));
    s_tts_initialized = true;
}

static esp_err_t cap_openai_tts_download_event_handler(esp_http_client_event_t *event)
{
    cap_openai_tts_download_t *dl = (cap_openai_tts_download_t *)event->user_data;

    if (!dl || event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }
    if (fwrite(event->data, 1, (size_t)event->data_len, dl->file) != (size_t)event->data_len) {
        ESP_LOGE(TAG, "TTS cache write failed after %u bytes: errno=%d",
                 (unsigned int)dl->bytes_written, errno);
        dl->error = true;
        return ESP_FAIL;
    }
    dl->bytes_written += (size_t)event->data_len;
    return ESP_OK;
}

/* Transport-level failures (connect/timeout/reset) and 5xx responses are
 * worth a short retry; 4xx (bad key, bad request) will just fail again. */
static bool cap_tts_is_transient_error(esp_err_t err, int status)
{
    if (err != ESP_OK) {
        return true;
    }
    return status == 0 || status >= 500;
}

static bool cap_tts_is_known_groq_voice(const char *voice)
{
    static const char *const voices[] = { "autumn", "diana", "hannah", "austin", "daniel", "troy" };

    for (size_t i = 0; i < sizeof(voices) / sizeof(voices[0]); i++) {
        if (strcasecmp(voice, voices[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Truncates text to at most max_len bytes on a UTF-8 boundary, appending an
 * ellipsis if it was cut short. Caller owns the returned buffer. */
static char *cap_tts_truncate_text(const char *text, size_t max_len)
{
    size_t prefix_len;
    char *out;

    if (!text) {
        return NULL;
    }
    if (strlen(text) <= max_len) {
        return strdup(text);
    }

    prefix_len = claw_utils_utf8_prefix_len(text, max_len);
    out = calloc(1, prefix_len + 8);
    if (!out) {
        return NULL;
    }
    memcpy(out, text, prefix_len);
    strcat(out, "\xe2\x80\xa6"); /* UTF-8 "…" */
    return out;
}

/**
 * @return ".ogg" (Opus) for OpenAI or ".wav" for Groq, since Groq's TTS API
 *         only supports the "wav" response_format (no Opus/ogg option).
 */
static esp_err_t cap_openai_tts_synthesize(const char *text,
                                           char *out_path,
                                           size_t out_path_size,
                                           const char **out_extension)
{
    cJSON *body = NULL;
    char *body_json = NULL;
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client = NULL;
    cap_openai_tts_download_t dl = {0};
    char auth_header[CAP_TTS_API_KEY_LEN + 16];
    FILE *file = NULL;
    const char *url;
    const char *model;
    const char *voice;
    const char *response_format;
    const char *extension;
    int status;
    esp_err_t err;

    if (!text || !text[0] || !out_path) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tts.api_key[0] == '\0') {
        ESP_LOGW(TAG, "TTS API key is not configured");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_tts.cache_dir[0] == '\0') {
        ESP_LOGW(TAG, "TTS cache directory is not configured");
        return ESP_ERR_INVALID_STATE;
    }

    if (cap_tts_is_groq()) {
        url = CAP_TTS_GROQ_API_URL;
        model = (strncmp(s_tts.model, "canopylabs/", 11) == 0) ? s_tts.model : CAP_TTS_GROQ_DEFAULT_MODEL;
        voice = (s_tts.voice[0] && cap_tts_is_known_groq_voice(s_tts.voice)) ? s_tts.voice : CAP_TTS_GROQ_DEFAULT_VOICE;
        response_format = "wav";
        extension = ".wav";
    } else {
        url = CAP_TTS_OPENAI_API_URL;
        model = s_tts.model[0] ? s_tts.model : CAP_TTS_OPENAI_DEFAULT_MODEL;
        voice = s_tts.voice[0] ? s_tts.voice : CAP_TTS_OPENAI_DEFAULT_VOICE;
        response_format = "opus";
        extension = ".ogg";
    }
    *out_extension = extension;

    snprintf(out_path, out_path_size, "%s/tts_%08" PRIx32 "%s",
             s_tts.cache_dir, (uint32_t)esp_random(), extension);

    {
        size_t max_text_len = cap_tts_is_groq() ? CAP_TTS_MAX_TEXT_LEN_GROQ : CAP_TTS_MAX_TEXT_LEN_OPENAI;
        char *bounded_text = cap_tts_truncate_text(text, max_text_len);
        bool offer_low_sample_rate = cap_tts_is_groq();
        bool sample_rate_rejected = false;
        int transient_retries_left = CAP_TTS_TRANSIENT_RETRIES;
        int attempt;

        if (!bounded_text) {
            return ESP_ERR_NO_MEM;
        }

        /* Groq gets up to three attempts: first with a lower sample_rate to
         * shrink the wav payload, then (only on a 4xx, meaning the field was
         * rejected) a plain retry at the model's default rate. Either flow
         * (and the single-attempt OpenAI flow) also gets one extra retry on
         * a transient transport/5xx failure. */
        for (attempt = 0; attempt < 3; attempt++) {
            bool with_sample_rate = offer_low_sample_rate && !sample_rate_rejected;

            body = cJSON_CreateObject();
            if (!body) {
                free(bounded_text);
                return ESP_ERR_NO_MEM;
            }
            cJSON_AddStringToObject(body, "model", model);
            cJSON_AddStringToObject(body, "voice", voice);
            cJSON_AddStringToObject(body, "input", bounded_text);
            cJSON_AddStringToObject(body, "response_format", response_format);
            if (with_sample_rate) {
                cJSON_AddNumberToObject(body, "sample_rate", CAP_TTS_GROQ_SAMPLE_RATE);
            }
            body_json = cJSON_PrintUnformatted(body);
            cJSON_Delete(body);
            body = NULL;
            if (!body_json) {
                free(bounded_text);
                return ESP_ERR_NO_MEM;
            }

            file = fopen(out_path, "wb");
            if (!file) {
                ESP_LOGW(TAG, "Failed to open %s for writing: errno=%d", out_path, errno);
                free(body_json);
                free(bounded_text);
                return ESP_FAIL;
            }
            memset(&dl, 0, sizeof(dl));
            dl.file = file;

            config = (esp_http_client_config_t){0};
            config.url = url;
            config.method = HTTP_METHOD_POST;
            config.event_handler = cap_openai_tts_download_event_handler;
            config.user_data = &dl;
            config.timeout_ms = CAP_TTS_HTTP_TIMEOUT_MS;
            config.buffer_size = 4096;
            config.buffer_size_tx = 1024;
            config.crt_bundle_attach = esp_crt_bundle_attach;
#ifdef CONFIG_HTTP_REUSE_ENABLE
            config.keep_alive_enable = true;
#endif

            client = esp_http_client_init(&config);
            if (!client) {
                fclose(file);
                remove(out_path);
                free(body_json);
                free(bounded_text);
                return ESP_FAIL;
            }

            snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_tts.api_key);
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_header(client, "Authorization", auth_header);
            esp_http_client_set_post_field(client, body_json, strlen(body_json));

            err = esp_http_client_perform(client);
            status = esp_http_client_get_status_code(client);
            esp_http_client_cleanup(client);
            fclose(file);
            free(body_json);
            body_json = NULL;

            if (err == ESP_OK && status == 200 && !dl.error && dl.bytes_written > 0) {
                free(bounded_text);
                return ESP_OK;
            }

            if (with_sample_rate && err == ESP_OK && status >= 400 && status < 500) {
                ESP_LOGW(TAG,
                        "Groq TTS rejected sample_rate=%d override (http=%d), retrying at default rate",
                        CAP_TTS_GROQ_SAMPLE_RATE, status);
                sample_rate_rejected = true;
                remove(out_path);
                continue;
            }

            if (transient_retries_left > 0 && cap_tts_is_transient_error(err, status)) {
                ESP_LOGW(TAG, "TTS request attempt %d failed (provider=%s err=%s http=%d), retrying",
                         attempt + 1, s_tts.provider, esp_err_to_name(err), status);
                transient_retries_left--;
                remove(out_path);
                vTaskDelay(pdMS_TO_TICKS(CAP_TTS_RETRY_BACKOFF_MS));
                continue;
            }

            ESP_LOGE(TAG, "TTS request failed: provider=%s err=%s http=%d bytes=%u",
                     s_tts.provider, esp_err_to_name(err), status, (unsigned int)dl.bytes_written);
            remove(out_path);
            free(bounded_text);
            return err != ESP_OK ? err : ESP_FAIL;
        }

        free(bounded_text);
        return ESP_FAIL;
    }
}

static const char *cap_openai_tts_resolve_waiting_send_cap(const char *channel)
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

static void cap_openai_tts_send_waiting_message(const char *channel, const char *chat_id)
{
    const char *send_cap = cap_openai_tts_resolve_waiting_send_cap(channel);
    const char *message = NULL;
    cJSON *body = NULL;
    char *body_json = NULL;
    char output[64];
    claw_cap_call_context_t call_ctx = {0};

    if (!send_cap || !chat_id || !chat_id[0]) {
        return;
    }

    message = s_tts_waiting_messages[esp_random() % (sizeof(s_tts_waiting_messages) / sizeof(s_tts_waiting_messages[0]))];

    body = cJSON_CreateObject();
    if (!body) {
        return;
    }
    cJSON_AddStringToObject(body, "chat_id", chat_id);
    cJSON_AddStringToObject(body, "message", message);
    body_json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_json) {
        return;
    }

    call_ctx.channel = channel;
    call_ctx.chat_id = chat_id;
    call_ctx.source_cap = "cap_openai_tts";
    call_ctx.caller = CLAW_CAP_CALLER_SYSTEM;
    claw_cap_call(send_cap, body_json, &call_ctx, output, sizeof(output));
    free(body_json);
}

static const char *cap_openai_tts_resolve_send_cap(const char *channel, const char *extension)
{
    if (!channel || strcmp(channel, "telegram") != 0) {
        return NULL;
    }
    return strcmp(extension, ".ogg") == 0 ? "tg_send_voice" : "tg_send_audio";
}

/* Length in bytes of the next chunk of text, at most max_len, preferring to
 * end after a sentence/paragraph break, then after whitespace, then on a
 * UTF-8 boundary. */
static size_t cap_tts_next_chunk_len(const char *text, size_t max_len)
{
    size_t window;
    size_t min_end;
    size_t whitespace_end = 0;

    if (strlen(text) <= max_len) {
        return strlen(text);
    }

    window = claw_utils_utf8_prefix_len(text, max_len);
    min_end = window * CAP_TTS_CHUNK_MIN_FILL_PCT / 100;
    for (size_t i = window; i > min_end; i--) {
        char prev = text[i - 1];
        char cur = text[i];

        if (prev == '\n' || ((prev == '.' || prev == '!' || prev == '?') && (cur == ' ' || cur == '\n'))) {
            return i;
        }
        if (whitespace_end == 0 && (cur == ' ' || cur == '\n')) {
            whitespace_end = i;
        }
    }
    return whitespace_end > 0 ? whitespace_end : window;
}

/* Copies text[0..len) without leading/trailing whitespace. */
static char *cap_tts_dup_trimmed(const char *text, size_t len)
{
    char *out;

    while (len > 0 && isspace((unsigned char)*text)) {
        text++;
        len--;
    }
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        len--;
    }
    out = malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, text, len);
    out[len] = '\0';
    return out;
}

static esp_err_t cap_openai_tts_speak_chunk(const char *channel,
                                            const char *chat_id,
                                            const char *text,
                                            char *output,
                                            size_t output_size)
{
    const char *extension = NULL;
    const char *send_cap;
    char path[CAP_TTS_PATH_LEN] = {0};
    cJSON *send_input;
    char *send_input_json;
    claw_cap_call_context_t send_ctx = {0};
    esp_err_t err;

    err = cap_openai_tts_synthesize(text, path, sizeof(path), &extension);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: TTS synthesis failed (%s)", esp_err_to_name(err));
        return err;
    }

    send_cap = cap_openai_tts_resolve_send_cap(channel, extension);
    if (!send_cap) {
        remove(path);
        snprintf(output, output_size, "Error: voice replies are not supported on channel %s", channel);
        return ESP_ERR_NOT_SUPPORTED;
    }

    send_input = cJSON_CreateObject();
    if (!send_input) {
        remove(path);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(send_input, "chat_id", chat_id);
    cJSON_AddStringToObject(send_input, "path", path);
    send_input_json = cJSON_PrintUnformatted(send_input);
    cJSON_Delete(send_input);
    if (!send_input_json) {
        remove(path);
        return ESP_ERR_NO_MEM;
    }

    send_ctx.channel = channel;
    send_ctx.chat_id = chat_id;
    send_ctx.source_cap = "cap_openai_tts";
    send_ctx.caller = CLAW_CAP_CALLER_SYSTEM;
    err = claw_cap_call(send_cap, send_input_json, &send_ctx, output, output_size);
    free(send_input_json);
    remove(path);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send TTS audio via %s: %s", send_cap, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t cap_openai_tts_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output,
                                        size_t output_size)
{
    cJSON *root = NULL;
    cJSON *text_json;
    cJSON *chat_id_json;
    cJSON *channel_json;
    const char *text = NULL;
    const char *chat_id = NULL;
    const char *channel = NULL;
    size_t max_chunk_len;
    int chunks_sent = 0;
    esp_err_t err = ESP_OK;

    root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    text_json = cJSON_GetObjectItem(root, "text");
    chat_id_json = cJSON_GetObjectItem(root, "chat_id");
    channel_json = cJSON_GetObjectItem(root, "channel");

    if (cJSON_IsString(text_json) && text_json->valuestring[0]) {
        text = text_json->valuestring;
    }
    if (cJSON_IsString(chat_id_json) && chat_id_json->valuestring[0]) {
        chat_id = chat_id_json->valuestring;
    } else if (ctx && ctx->chat_id && ctx->chat_id[0]) {
        chat_id = ctx->chat_id;
    }
    if (cJSON_IsString(channel_json) && channel_json->valuestring[0]) {
        channel = channel_json->valuestring;
    } else if (ctx && ctx->channel && ctx->channel[0]) {
        channel = ctx->channel;
    }

    if (!text || !chat_id || !channel) {
        cJSON_Delete(root);
        snprintf(output, output_size,
                "Error: text, chat_id and channel are required (chat_id/channel may come from ctx)");
        return ESP_ERR_INVALID_ARG;
    }

    cap_openai_tts_send_waiting_message(channel, chat_id);

    max_chunk_len = cap_tts_is_groq() ? CAP_TTS_MAX_TEXT_LEN_GROQ : CAP_TTS_MAX_TEXT_LEN_OPENAI;
    while (*text && chunks_sent < CAP_TTS_MAX_CHUNKS) {
        size_t chunk_len = cap_tts_next_chunk_len(text, max_chunk_len);
        char *chunk = cap_tts_dup_trimmed(text, chunk_len);

        text += chunk_len;
        if (!chunk) {
            err = ESP_ERR_NO_MEM;
            break;
        }
        if (chunk[0] == '\0') {
            free(chunk);
            continue;
        }
        err = cap_openai_tts_speak_chunk(channel, chat_id, chunk, output, output_size);
        free(chunk);
        if (err != ESP_OK) {
            break;
        }
        chunks_sent++;
    }
    cJSON_Delete(root);

    if (err != ESP_OK) {
        if (chunks_sent > 0) {
            ESP_LOGW(TAG, "TTS stopped after %d chunk(s): %s", chunks_sent, esp_err_to_name(err));
            snprintf(output, output_size, "voice reply partially sent (%d part(s)); then failed: %s",
                     chunks_sent, esp_err_to_name(err));
        }
        return err;
    }

    if (*text) {
        snprintf(output, output_size,
                 "voice reply sent in %d part(s); the rest of the text was too long and was not spoken",
                 chunks_sent);
    } else {
        snprintf(output, output_size, "voice reply already sent to user (%d part(s))", chunks_sent);
    }
    return ESP_OK;
}

static const claw_cap_descriptor_t s_tts_descriptors[] = {
    {
        .id = "tts_speak",
        .name = "tts_speak",
        .family = "system",
        .description = "Synthesize speech for text via the configured TTS provider (OpenAI or Groq) "
                       "and send it back to the chat as voice/audio message(s). Long text is split "
                       "automatically into several consecutive audios, so pass the full text.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"},\"chat_id\":{\"type\":\"string\"},\"channel\":{\"type\":\"string\"}},\"required\":[\"text\"]}",
        .execute = cap_openai_tts_execute,
    },
};

static const claw_cap_group_t s_tts_group = {
    .group_id = "cap_openai_tts",
    .descriptors = s_tts_descriptors,
    .descriptor_count = sizeof(s_tts_descriptors) / sizeof(s_tts_descriptors[0]),
};

esp_err_t cap_openai_tts_register_group(void)
{
    cap_openai_tts_init_defaults();

    if (claw_cap_group_exists(s_tts_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_tts_group);
}

esp_err_t cap_openai_tts_set_api_key(const char *api_key)
{
    cap_openai_tts_init_defaults();
    if (!api_key) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_tts.api_key, api_key, sizeof(s_tts.api_key));
    return ESP_OK;
}

esp_err_t cap_openai_tts_set_voice(const char *voice)
{
    cap_openai_tts_init_defaults();
    strlcpy(s_tts.voice, (voice && voice[0]) ? voice : CAP_TTS_OPENAI_DEFAULT_VOICE, sizeof(s_tts.voice));
    return ESP_OK;
}

esp_err_t cap_openai_tts_set_model(const char *model)
{
    cap_openai_tts_init_defaults();
    strlcpy(s_tts.model, (model && model[0]) ? model : CAP_TTS_OPENAI_DEFAULT_MODEL, sizeof(s_tts.model));
    return ESP_OK;
}

esp_err_t cap_openai_tts_set_provider(const char *provider)
{
    cap_openai_tts_init_defaults();
    strlcpy(s_tts.provider, (provider && strcmp(provider, "groq") == 0) ? "groq" : "openai", sizeof(s_tts.provider));
    return ESP_OK;
}

esp_err_t cap_openai_tts_set_cache_dir(const char *cache_dir)
{
    struct stat st = {0};

    cap_openai_tts_init_defaults();
    if (!cache_dir || !cache_dir[0]) {
        s_tts.cache_dir[0] = '\0';
        return ESP_OK;
    }

    if (stat(cache_dir, &st) != 0) {
        if (mkdir(cache_dir, 0755) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "Failed to create TTS cache dir %s: errno=%d", cache_dir, errno);
            return ESP_FAIL;
        }
    }

    strlcpy(s_tts.cache_dir, cache_dir, sizeof(s_tts.cache_dir));
    return ESP_OK;
}
