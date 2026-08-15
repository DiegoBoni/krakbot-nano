/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_groq_transcribe.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_event_publisher.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cap_groq_transcribe";

static const char *const s_waiting_messages[] = {
    "🎙️ Tratando de entenderte...",
    "🧩 Descifrando chino mandarín...",
    "🐙 Afinando el oído...",
    "🔮 Interpretando tus murmullos...",
    "🎧 Escuchando con atención (o eso intento)...",
    "🕵️ Investigando qué dijiste...",
    "🌀 Procesando ondas sonoras misteriosas...",
    "📡 Decodificando transmisión de audio...",
};

#define CAP_GROQ_API_URL              "https://api.groq.com/openai/v1/audio/transcriptions"
#define CAP_GROQ_DEFAULT_MODEL        "whisper-large-v3-turbo"
#define CAP_GROQ_HTTP_RESP_INIT       1024
#define CAP_GROQ_HTTP_TIMEOUT_MS      30000
#define CAP_GROQ_MULTIPART_BOUNDARY   "----cap_groq_transcribe_boundary"
#define CAP_GROQ_API_KEY_LEN          128
#define CAP_GROQ_MODEL_LEN            64
#define CAP_GROQ_MAX_ATTEMPTS         2
#define CAP_GROQ_RETRY_BACKOFF_MS     400

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} cap_groq_http_resp_t;

typedef struct {
    char api_key[CAP_GROQ_API_KEY_LEN];
    char model[CAP_GROQ_MODEL_LEN];
} cap_groq_state_t;

static EXT_RAM_BSS_ATTR cap_groq_state_t s_groq;
static bool s_groq_initialized;

static void cap_groq_init_defaults(void)
{
    if (s_groq_initialized) {
        return;
    }

    strlcpy(s_groq.model, CAP_GROQ_DEFAULT_MODEL, sizeof(s_groq.model));
    s_groq_initialized = true;
}

static esp_err_t cap_groq_http_event_handler(esp_http_client_event_t *event)
{
    cap_groq_http_resp_t *resp = (cap_groq_http_resp_t *)event->user_data;

    if (!resp || event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }

    if (resp->len + (size_t)event->data_len + 1 > resp->cap) {
        char *tmp = NULL;
        size_t new_cap = resp->cap * 2;

        if (new_cap < resp->len + (size_t)event->data_len + 1) {
            new_cap = resp->len + (size_t)event->data_len + 1;
        }

        tmp = realloc(resp->buf, new_cap);
        if (!tmp) {
            return ESP_ERR_NO_MEM;
        }

        resp->buf = tmp;
        resp->cap = new_cap;
    }

    memcpy(resp->buf + resp->len, event->data, event->data_len);
    resp->len += (size_t)event->data_len;
    resp->buf[resp->len] = '\0';
    return ESP_OK;
}

/* Transport-level failures (connect/timeout/reset) and 5xx responses are
 * worth a short retry; 4xx (bad key, bad request) will just fail again. */
static bool cap_groq_is_transient_error(esp_err_t err, int status)
{
    if (err != ESP_OK) {
        return true;
    }
    return status == 0 || status >= 500;
}

static const char *cap_groq_basename(const char *path)
{
    const char *slash = NULL;

    if (!path || !path[0]) {
        return "audio.bin";
    }

    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static esp_err_t cap_groq_http_client_write_all(esp_http_client_handle_t client,
                                                 const char *data,
                                                 size_t len)
{
    size_t total = 0;

    if (!client || (!data && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    while (total < len) {
        int written = esp_http_client_write(client, data + total, (int)(len - total));

        if (written <= 0) {
            return ESP_FAIL;
        }

        total += (size_t)written;
    }

    return ESP_OK;
}

static esp_err_t cap_groq_stream_file_to_http_client(esp_http_client_handle_t client, FILE *file)
{
    char buf[1024];

    if (!client || !file) {
        return ESP_ERR_INVALID_ARG;
    }

    while (!feof(file)) {
        size_t nread = fread(buf, 1, sizeof(buf), file);

        if (nread > 0) {
            esp_err_t err = cap_groq_http_client_write_all(client, buf, nread);
            if (err != ESP_OK) {
                return err;
            }
        }

        if (ferror(file)) {
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

static esp_err_t cap_groq_transcribe_file(const char *path, char **out_text)
{
    struct stat st;
    FILE *file = NULL;
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client = NULL;
    cap_groq_http_resp_t resp = {0};
    char auth_header[CAP_GROQ_API_KEY_LEN + 16];
    char content_type[128];
    char part_model[192];
    char part_file[512];
    char closing[64];
    const char *file_name = NULL;
    size_t content_length;
    int part_model_len;
    int part_file_len;
    int closing_len;
    int status;
    esp_err_t err = ESP_FAIL;

    if (!path || !path[0] || !out_text) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_groq.api_key[0] == '\0') {
        ESP_LOGW(TAG, "Groq API key is not configured");
        return ESP_ERR_INVALID_STATE;
    }
    if (stat(path, &st) != 0) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (!S_ISREG(st.st_mode) || st.st_size <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    file_name = cap_groq_basename(path);

    part_model_len = snprintf(part_model, sizeof(part_model),
                              "--" CAP_GROQ_MULTIPART_BOUNDARY "\r\n"
                              "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
                              "%s\r\n",
                              s_groq.model[0] ? s_groq.model : CAP_GROQ_DEFAULT_MODEL);
    if (part_model_len <= 0 || part_model_len >= (int)sizeof(part_model)) {
        return ESP_ERR_INVALID_SIZE;
    }

    part_file_len = snprintf(part_file, sizeof(part_file),
                             "--" CAP_GROQ_MULTIPART_BOUNDARY "\r\n"
                             "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
                             "Content-Type: application/octet-stream\r\n\r\n",
                             file_name);
    if (part_file_len <= 0 || part_file_len >= (int)sizeof(part_file)) {
        return ESP_ERR_INVALID_SIZE;
    }

    closing_len = snprintf(closing, sizeof(closing),
                           "\r\n--" CAP_GROQ_MULTIPART_BOUNDARY "--\r\n");
    if (closing_len <= 0 || closing_len >= (int)sizeof(closing)) {
        return ESP_ERR_INVALID_SIZE;
    }

    content_length = (size_t)part_model_len + (size_t)part_file_len +
                     (size_t)st.st_size + (size_t)closing_len;

    file = fopen(path, "rb");
    if (!file) {
        return ESP_FAIL;
    }

    resp.buf = calloc(1, CAP_GROQ_HTTP_RESP_INIT);
    if (!resp.buf) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    resp.cap = CAP_GROQ_HTTP_RESP_INIT;

    snprintf(content_type, sizeof(content_type),
             "multipart/form-data; boundary=" CAP_GROQ_MULTIPART_BOUNDARY);
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_groq.api_key);

    for (int attempt = 0; attempt < CAP_GROQ_MAX_ATTEMPTS; attempt++) {
        resp.len = 0;
        if (resp.buf) {
            resp.buf[0] = '\0';
        }
        rewind(file);

        config = (esp_http_client_config_t){0};
        config.url = CAP_GROQ_API_URL;
        config.method = HTTP_METHOD_POST;
        config.event_handler = cap_groq_http_event_handler;
        config.user_data = &resp;
        config.timeout_ms = CAP_GROQ_HTTP_TIMEOUT_MS;
        config.buffer_size = 2048;
        config.buffer_size_tx = 2048;
        config.crt_bundle_attach = esp_crt_bundle_attach;
#ifdef CONFIG_HTTP_REUSE_ENABLE
        config.keep_alive_enable = true;
#endif

        client = esp_http_client_init(&config);
        if (!client) {
            err = ESP_FAIL;
            status = 0;
            break;
        }

        esp_http_client_set_header(client, "Content-Type", content_type);
        esp_http_client_set_header(client, "Authorization", auth_header);

        err = esp_http_client_open(client, (int)content_length);
        if (err == ESP_OK) {
            err = cap_groq_http_client_write_all(client, part_model, (size_t)part_model_len);
        }
        if (err == ESP_OK) {
            err = cap_groq_http_client_write_all(client, part_file, (size_t)part_file_len);
        }
        if (err == ESP_OK) {
            err = cap_groq_stream_file_to_http_client(client, file);
        }
        if (err == ESP_OK) {
            err = cap_groq_http_client_write_all(client, closing, (size_t)closing_len);
        }
        if (err == ESP_OK) {
            if (esp_http_client_fetch_headers(client) < 0) {
                err = ESP_FAIL;
            }
        }

        status = esp_http_client_get_status_code(client);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        client = NULL;

        if (err == ESP_OK && status == 200) {
            break;
        }

        if (attempt + 1 < CAP_GROQ_MAX_ATTEMPTS && cap_groq_is_transient_error(err, status)) {
            ESP_LOGW(TAG, "Groq transcription attempt %d failed (err=%s http=%d), retrying",
                     attempt + 1, esp_err_to_name(err), status);
            vTaskDelay(pdMS_TO_TICKS(CAP_GROQ_RETRY_BACKOFF_MS));
            continue;
        }

        break;
    }

    fclose(file);

    if (err != ESP_OK) {
        free(resp.buf);
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "Groq transcription failed: http=%d body=%s", status, resp.buf ? resp.buf : "");
        free(resp.buf);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp.buf);
    free(resp.buf);
    if (!root) {
        return ESP_FAIL;
    }

    cJSON *text_json = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(text_json) || !text_json->valuestring) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    *out_text = strdup(text_json->valuestring);
    cJSON_Delete(root);
    return *out_text ? ESP_OK : ESP_ERR_NO_MEM;
}

static const char *cap_groq_resolve_send_cap(const char *channel)
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

static void cap_groq_send_waiting_message(const char *channel, const char *chat_id)
{
    const char *send_cap = cap_groq_resolve_send_cap(channel);
    const char *message = NULL;
    cJSON *body = NULL;
    char *body_json = NULL;
    char output[64];
    claw_cap_call_context_t call_ctx = {0};

    if (!send_cap || !chat_id || !chat_id[0]) {
        return;
    }

    message = s_waiting_messages[esp_random() % (sizeof(s_waiting_messages) / sizeof(s_waiting_messages[0]))];

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
    call_ctx.source_cap = "cap_groq_transcribe";
    call_ctx.caller = CLAW_CAP_CALLER_SYSTEM;
    claw_cap_call(send_cap, body_json, &call_ctx, output, sizeof(output));
    free(body_json);
}

static esp_err_t cap_groq_transcribe_execute(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output,
                                             size_t output_size)
{
    cJSON *root = NULL;
    cJSON *saved_path_json;
    cJSON *chat_id_json;
    cJSON *channel_json;
    cJSON *sender_id_json;
    cJSON *message_id_json;
    const char *saved_path = NULL;
    const char *chat_id = NULL;
    const char *channel = NULL;
    const char *sender_id = NULL;
    const char *message_id = NULL;
    char *text = NULL;
    esp_err_t err;

    root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    saved_path_json = cJSON_GetObjectItem(root, "saved_path");
    chat_id_json = cJSON_GetObjectItem(root, "chat_id");
    channel_json = cJSON_GetObjectItem(root, "channel");
    sender_id_json = cJSON_GetObjectItem(root, "sender_id");
    message_id_json = cJSON_GetObjectItem(root, "message_id");

    if (cJSON_IsString(saved_path_json) && saved_path_json->valuestring[0]) {
        saved_path = saved_path_json->valuestring;
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
    if (cJSON_IsString(sender_id_json) && sender_id_json->valuestring[0]) {
        sender_id = sender_id_json->valuestring;
    }
    if (cJSON_IsString(message_id_json) && message_id_json->valuestring[0]) {
        message_id = message_id_json->valuestring;
    }

    if (!saved_path || !chat_id || !channel) {
        cJSON_Delete(root);
        snprintf(output, output_size,
                "Error: saved_path, chat_id and channel are required (chat_id/channel may come from ctx)");
        return ESP_ERR_INVALID_ARG;
    }

    cap_groq_send_waiting_message(channel, chat_id);

    err = cap_groq_transcribe_file(saved_path, &text);
    if (err != ESP_OK || !text) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: transcription failed (%s)", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Transcribed %s chat=%s: %.64s%s",
             saved_path, chat_id, text, strlen(text) > 64 ? "..." : "");

    err = claw_event_router_publish_message("cap_groq_transcribe",
                                            channel,
                                            chat_id,
                                            text,
                                            sender_id,
                                            message_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to republish transcribed text as message: %s", esp_err_to_name(err));
    }

    snprintf(output, output_size, "%s", text);
    free(text);
    cJSON_Delete(root);
    return err;
}

static const claw_cap_descriptor_t s_groq_descriptors[] = {
    {
        .id = "groq_transcribe",
        .name = "groq_transcribe",
        .family = "system",
        .description = "Transcribe a local audio file via the Groq Whisper API and route the "
                       "result back into the chat as a text message.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"saved_path\":{\"type\":\"string\"},\"chat_id\":{\"type\":\"string\"},\"channel\":{\"type\":\"string\"},\"sender_id\":{\"type\":\"string\"},\"message_id\":{\"type\":\"string\"}},\"required\":[\"saved_path\"]}",
        .execute = cap_groq_transcribe_execute,
    },
};

static const claw_cap_group_t s_groq_group = {
    .group_id = "cap_groq_transcribe",
    .descriptors = s_groq_descriptors,
    .descriptor_count = sizeof(s_groq_descriptors) / sizeof(s_groq_descriptors[0]),
};

esp_err_t cap_groq_transcribe_register_group(void)
{
    cap_groq_init_defaults();

    if (claw_cap_group_exists(s_groq_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_groq_group);
}

esp_err_t cap_groq_transcribe_set_api_key(const char *api_key)
{
    cap_groq_init_defaults();

    if (!api_key) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_groq.api_key, api_key, sizeof(s_groq.api_key));
    return ESP_OK;
}

esp_err_t cap_groq_transcribe_set_model(const char *model)
{
    cap_groq_init_defaults();

    if (!model || !model[0]) {
        strlcpy(s_groq.model, CAP_GROQ_DEFAULT_MODEL, sizeof(s_groq.model));
        return ESP_OK;
    }

    strlcpy(s_groq.model, model, sizeof(s_groq.model));
    return ESP_OK;
}
