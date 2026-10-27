/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mcp_engine.h"
#include "esp_mcp_mgr.h"

#include "cap_mcp_client_priv.h"
#include "cap_mcp_registry.h"

#define CAP_MCP_HTTP_TIMEOUT_MS    20000
#define CAP_MCP_CONNECT_MAX_ATTEMPTS   3
#define CAP_MCP_CONNECT_RETRY_DELAY_MS 700

static const char *TAG = "cap_mcp_client";

typedef struct {
    cJSON *result;
    esp_err_t err;
} cap_mcp_response_ctx_t;

typedef struct {
    cap_mcp_auth_type_t type;
    char username[CAP_MCP_SERVER_USERNAME_LEN];
    char secret[CAP_MCP_SERVER_SECRET_LEN];
} cap_mcp_auth_t;

/* Resolves "server_id" (a saved registry entry) into server_url/endpoint/auth when the
 * caller did not pass an explicit "server_url". Leaves auth->type == CAP_MCP_AUTH_NONE
 * when server_url was given directly (ad hoc calls carry no stored credentials). */
static esp_err_t cap_mcp_resolve_server_id(cJSON *input,
                                           char *server_url_buf,
                                           size_t server_url_buf_size,
                                           char *endpoint_buf,
                                           size_t endpoint_buf_size,
                                           cap_mcp_auth_t *auth)
{
    cJSON *server_id_item = cJSON_GetObjectItem(input, "server_id");
    if (!cJSON_IsString(server_id_item) || !server_id_item->valuestring[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    cap_mcp_server_entry_t entry = {0};
    esp_err_t err = cap_mcp_registry_get(server_id_item->valuestring, &entry);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "server_id '%s' not found in registry (%s)", server_id_item->valuestring, esp_err_to_name(err));
        return err;
    }
    if (!entry.enabled) {
        ESP_LOGW(TAG, "server_id '%s' is disabled", server_id_item->valuestring);
        return ESP_ERR_INVALID_STATE;
    }

    strlcpy(server_url_buf, entry.server_url, server_url_buf_size);
    if (endpoint_buf && endpoint_buf_size > 0) {
        strlcpy(endpoint_buf, entry.endpoint[0] ? entry.endpoint : MCP_MDNS_DEFAULT_ENDPOINT, endpoint_buf_size);
    }
    if (auth) {
        auth->type = entry.auth_type;
        strlcpy(auth->username, entry.username, sizeof(auth->username));
        strlcpy(auth->secret, entry.secret, sizeof(auth->secret));
    }
    return ESP_OK;
}

static esp_err_t cap_mcp_parse_common_input(const char *input_json,
                                            char *server_url_buf,
                                            size_t server_url_buf_size,
                                            char *endpoint_buf,
                                            size_t endpoint_buf_size,
                                            char *cursor_buf,
                                            size_t cursor_buf_size,
                                            char *tool_name_buf,
                                            size_t tool_name_buf_size,
                                            cJSON **arguments_out,
                                            cap_mcp_auth_t *auth)
{
    cJSON *input = cJSON_Parse(input_json);

    if (!input || !cJSON_IsObject(input)) {
        cJSON_Delete(input);
        return ESP_ERR_INVALID_ARG;
    }

    if (auth) {
        memset(auth, 0, sizeof(*auth));
    }

    cJSON *server_url_item = cJSON_GetObjectItem(input, "server_url");
    if (cJSON_IsString(server_url_item) && server_url_item->valuestring[0]) {
        strlcpy(server_url_buf, server_url_item->valuestring, server_url_buf_size);

        if (endpoint_buf && endpoint_buf_size > 0) {
            const char *endpoint = MCP_MDNS_DEFAULT_ENDPOINT;
            cJSON *endpoint_item = cJSON_GetObjectItem(input, "endpoint");
            if (cJSON_IsString(endpoint_item) && endpoint_item->valuestring[0]) {
                endpoint = endpoint_item->valuestring;
            }
            strlcpy(endpoint_buf, endpoint, endpoint_buf_size);
        }
    } else {
        esp_err_t resolve_err = cap_mcp_resolve_server_id(input, server_url_buf, server_url_buf_size,
                                                           endpoint_buf, endpoint_buf_size, auth);
        if (resolve_err != ESP_OK) {
            cJSON_Delete(input);
            return resolve_err == ESP_ERR_NOT_FOUND || resolve_err == ESP_ERR_INVALID_STATE
                       ? resolve_err
                       : ESP_ERR_INVALID_ARG;
        }
    }

    if (cursor_buf && cursor_buf_size > 0) {
        cJSON *cursor_item = cJSON_GetObjectItem(input, "cursor");

        cursor_buf[0] = '\0';
        if (cJSON_IsString(cursor_item) && cursor_item->valuestring[0]) {
            strlcpy(cursor_buf, cursor_item->valuestring, cursor_buf_size);
        }
    }

    if (tool_name_buf && tool_name_buf_size > 0) {
        cJSON *tool_name_item = cJSON_GetObjectItem(input, "tool_name");
        if (!cJSON_IsString(tool_name_item) || !tool_name_item->valuestring[0]) {
            cJSON_Delete(input);
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(tool_name_buf, tool_name_item->valuestring, tool_name_buf_size);
    }

    if (arguments_out) {
        cJSON *arguments = cJSON_GetObjectItem(input, "arguments");

        if (!arguments || !cJSON_IsObject(arguments)) {
            *arguments_out = cJSON_CreateObject();
        } else {
            *arguments_out = cJSON_Duplicate(arguments, 1);
        }

        if (!*arguments_out) {
            cJSON_Delete(input);
            return ESP_ERR_NO_MEM;
        }
    }

    cJSON_Delete(input);
    return ESP_OK;
}

static esp_err_t cap_mcp_mgr_response_cb(int error_code,
                                         const char *ep_name,
                                         const char *resp_json,
                                         void *user_ctx,
                                         uint32_t jsonrpc_request_id)
{
    cap_mcp_response_ctx_t *ctx = (cap_mcp_response_ctx_t *)user_ctx;

    (void)ep_name;
    (void)jsonrpc_request_id;

    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(ctx->result);
    ctx->result = cJSON_CreateObject();
    if (!ctx->result) {
        ctx->err = ESP_ERR_NO_MEM;
        return ctx->err;
    }

    if (error_code == 0 || error_code == 1) {
        if (resp_json && resp_json[0]) {
            cJSON *parsed = cJSON_Parse(resp_json);

            if (!cJSON_IsObject(parsed)) {
                cJSON_Delete(parsed);
                cJSON_AddStringToObject(ctx->result, "error_message", "Invalid MCP response");
                ctx->err = ESP_FAIL;
                return ctx->err;
            }
            cJSON_Delete(ctx->result);
            ctx->result = parsed;
        }
        ctx->err = ESP_OK;
        return ESP_OK;
    }

    if (error_code < 0) {
        cJSON_AddStringToObject(ctx->result,
                                "error_message",
                                (resp_json && resp_json[0]) ? resp_json : "Unknown MCP protocol error");
        ctx->err = ESP_OK;
        return ESP_OK;
    }

    cJSON_AddStringToObject(ctx->result, "error_message", esp_err_to_name(error_code));
    ctx->err = ESP_OK;
    return ESP_OK;
}

static esp_err_t cap_mcp_init_remote_session(esp_mcp_mgr_handle_t mgr, const char *endpoint)
{
    cap_mcp_response_ctx_t ctx = {0};
    esp_mcp_mgr_req_t req = {
        .ep_name = endpoint,
        .cb = cap_mcp_mgr_response_cb,
        .user_ctx = &ctx,
        .u.init = {
            /* Pin to the 2024-11-05 MCP spec: newer negotiated versions make the SDK
             * auto-send "notifications/initialized" re-entrantly from inside this very
             * response callback, which corrupts the HTTP client and crashes the device
             * whenever that follow-up POST fails (e.g. on a flaky connection). Servers
             * that only support newer versions still generally accept this and either
             * echo it back or reply with their own (compatible) version. */
            .protocol_version = "2024-11-05",
            .name = "esp-claw",
            .version = "1.0.0",
            .title = "ESP-Claw",
        },
    };
    esp_err_t err = esp_mcp_mgr_post_info_init(mgr, &req);

    if (err == ESP_OK) {
        err = ctx.err;
    }
    cJSON_Delete(ctx.result);
    return err;
}

static esp_err_t cap_mcp_mgr_create(const char *server_url,
                                    const char *endpoint,
                                    const cap_mcp_auth_t *auth,
                                    esp_mcp_mgr_handle_t *mgr_out,
                                    esp_mcp_t **engine_out)
{
    esp_err_t err;
    esp_http_client_config_t http_config = {
        .url = server_url,
        .timeout_ms = CAP_MCP_HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .keep_alive_enable = true,
    };

    if (!server_url || !server_url[0] || !endpoint || !endpoint[0] || !mgr_out || !engine_out) {
        return ESP_ERR_INVALID_ARG;
    }

    if (auth && auth->type == CAP_MCP_AUTH_BASIC) {
        http_config.username = auth->username;
        http_config.password = auth->secret;
        http_config.auth_type = HTTP_AUTH_TYPE_BASIC;
    }

    *mgr_out = 0;
    *engine_out = NULL;

    /* esp_mcp_mgr_config_t.instance must reference a live protocol engine: without it,
     * esp_mcp_mgr_post_req() fails every request with "MCP instance not configured"
     * (ESP_ERR_INVALID_STATE), before any HTTP traffic is even attempted. */
    err = esp_mcp_create(engine_out);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_mcp_create failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_mcp_mgr_config_t mgr_config = {
        .transport = esp_mcp_transport_http_client,
        .config = &http_config,
        .instance = *engine_out,
    };

    err = esp_mcp_mgr_init(mgr_config, mgr_out);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_mcp_mgr_init failed for %s: %s", server_url, esp_err_to_name(err));
        esp_mcp_destroy(*engine_out);
        *engine_out = NULL;
        return err;
    }

    if (auth && auth->type == CAP_MCP_AUTH_BEARER && auth->secret[0]) {
        err = esp_mcp_mgr_set_bearer_token(*mgr_out, auth->secret);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_mcp_mgr_set_bearer_token failed: %s", esp_err_to_name(err));
            esp_mcp_mgr_deinit(*mgr_out);
            *mgr_out = 0;
            esp_mcp_destroy(*engine_out);
            *engine_out = NULL;
            return err;
        }
    }

    err = esp_mcp_mgr_start(*mgr_out);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_mcp_mgr_start failed: %s", esp_err_to_name(err));
        esp_mcp_mgr_deinit(*mgr_out);
        *mgr_out = 0;
        esp_mcp_destroy(*engine_out);
        *engine_out = NULL;
        return err;
    }

    err = esp_mcp_mgr_register_endpoint(*mgr_out, endpoint, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_mcp_mgr_register_endpoint('%s') failed: %s", endpoint, esp_err_to_name(err));
        esp_mcp_mgr_stop(*mgr_out);
        esp_mcp_mgr_deinit(*mgr_out);
        *mgr_out = 0;
        esp_mcp_destroy(*engine_out);
        *engine_out = NULL;
        return err;
    }

    err = cap_mcp_init_remote_session(*mgr_out, endpoint);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cap_mcp_init_remote_session failed: %s", esp_err_to_name(err));
        esp_mcp_mgr_stop(*mgr_out);
        esp_mcp_mgr_deinit(*mgr_out);
        *mgr_out = 0;
        esp_mcp_destroy(*engine_out);
        *engine_out = NULL;
    }
    return err;
}

/* Only connection-establishment failures are retried: the request never reached the
 * server, so repeating it cannot duplicate a side effect (e.g. sending an email twice). */
static bool cap_mcp_err_is_connect_failure(esp_err_t err)
{
    return err == ESP_ERR_HTTP_CONNECT || err == ESP_ERR_HTTP_CONNECTING;
}

static void cap_mcp_connect_retry_wait(const char *what, int attempt, esp_err_t err)
{
    ESP_LOGW(TAG, "%s attempt %d/%d failed (%s), retrying",
             what, attempt, CAP_MCP_CONNECT_MAX_ATTEMPTS, esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(CAP_MCP_CONNECT_RETRY_DELAY_MS));
}

static esp_err_t cap_mcp_mgr_create_retry(const char *server_url,
                                          const char *endpoint,
                                          const cap_mcp_auth_t *auth,
                                          esp_mcp_mgr_handle_t *mgr_out,
                                          esp_mcp_t **engine_out)
{
    esp_err_t err = ESP_FAIL;

    for (int attempt = 1; attempt <= CAP_MCP_CONNECT_MAX_ATTEMPTS; attempt++) {
        err = cap_mcp_mgr_create(server_url, endpoint, auth, mgr_out, engine_out);
        if (err == ESP_OK || !cap_mcp_err_is_connect_failure(err) ||
            attempt == CAP_MCP_CONNECT_MAX_ATTEMPTS) {
            break;
        }
        cap_mcp_connect_retry_wait("MCP session init", attempt, err);
    }
    return err;
}

static void cap_mcp_mgr_destroy(esp_mcp_mgr_handle_t mgr, esp_mcp_t *engine)
{
    if (mgr != 0) {
        esp_mcp_mgr_stop(mgr);
        esp_mcp_mgr_deinit(mgr);
    }
    if (engine) {
        esp_mcp_destroy(engine);
    }
}

static esp_err_t cap_mcp_capture_result(cap_mcp_response_ctx_t *ctx, cJSON **result_out)
{
    if (!ctx || !result_out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->err != ESP_OK) {
        cJSON_Delete(ctx->result);
        ctx->result = NULL;
        return ctx->err;
    }
    if (!ctx->result) {
        return ESP_FAIL;
    }

    *result_out = ctx->result;
    ctx->result = NULL;
    return ESP_OK;
}

esp_err_t cap_mcp_list_remote_tools(const char *input_json, cJSON **result_out)
{
    char server_url_buf[256];
    char endpoint_buf[64];
    char cursor_buf[128];
    cap_mcp_auth_t auth = {0};
    esp_mcp_mgr_handle_t mgr = 0;
    esp_mcp_t *engine = NULL;
    cap_mcp_response_ctx_t ctx = {0};
    esp_mcp_mgr_req_t req = {0};
    esp_err_t err;

    if (!input_json || !result_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *result_out = NULL;

    err = cap_mcp_parse_common_input(input_json,
                                     server_url_buf,
                                     sizeof(server_url_buf),
                                     endpoint_buf,
                                     sizeof(endpoint_buf),
                                     cursor_buf,
                                     sizeof(cursor_buf),
                                     NULL,
                                     0,
                                     NULL,
                                     &auth);
    if (err != ESP_OK) {
        return err;
    }

    err = cap_mcp_mgr_create_retry(server_url_buf, endpoint_buf, &auth, &mgr, &engine);
    if (err != ESP_OK) {
        return err;
    }

    req.ep_name = endpoint_buf;
    req.cb = cap_mcp_mgr_response_cb;
    req.user_ctx = &ctx;
    req.u.list.cursor = cursor_buf[0] ? cursor_buf : NULL;
    req.u.list.limit = -1;

    err = esp_mcp_mgr_post_tools_list(mgr, &req);
    cap_mcp_mgr_destroy(mgr, engine);
    if (err == ESP_OK) {
        err = cap_mcp_capture_result(&ctx, result_out);
    }
    cJSON_Delete(ctx.result);
    return err;
}

esp_err_t cap_mcp_call_remote_tool(const char *input_json, cJSON **result_out)
{
    char server_url_buf[256];
    char endpoint_buf[64];
    char tool_name_buf[128];
    cap_mcp_auth_t auth = {0};
    cJSON *arguments = NULL;
    char *arguments_json = NULL;
    esp_mcp_mgr_handle_t mgr = 0;
    esp_mcp_t *engine = NULL;
    cap_mcp_response_ctx_t ctx = {0};
    esp_mcp_mgr_req_t req = {0};
    esp_err_t err;

    if (!input_json || !result_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *result_out = NULL;

    err = cap_mcp_parse_common_input(input_json,
                                     server_url_buf,
                                     sizeof(server_url_buf),
                                     endpoint_buf,
                                     sizeof(endpoint_buf),
                                     NULL,
                                     0,
                                     tool_name_buf,
                                     sizeof(tool_name_buf),
                                     &arguments,
                                     &auth);
    if (err != ESP_OK) {
        cJSON_Delete(arguments);
        return err;
    }

    arguments_json = cJSON_PrintUnformatted(arguments);
    cJSON_Delete(arguments);
    if (!arguments_json) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_mcp_mgr_create_retry(server_url_buf, endpoint_buf, &auth, &mgr, &engine);
    if (err != ESP_OK) {
        cJSON_free(arguments_json);
        return err;
    }

    req.ep_name = endpoint_buf;
    req.cb = cap_mcp_mgr_response_cb;
    req.user_ctx = &ctx;
    req.u.call.tool_name = tool_name_buf;
    req.u.call.args_json = arguments_json;

    for (int attempt = 1;; attempt++) {
        err = esp_mcp_mgr_post_tools_call(mgr, &req);
        if (!cap_mcp_err_is_connect_failure(err) || attempt == CAP_MCP_CONNECT_MAX_ATTEMPTS) {
            break;
        }
        cap_mcp_connect_retry_wait("MCP tools/call", attempt, err);
    }
    cap_mcp_mgr_destroy(mgr, engine);
    cJSON_free(arguments_json);
    if (err == ESP_OK) {
        err = cap_mcp_capture_result(&ctx, result_out);
    }
    cJSON_Delete(ctx.result);
    return err;
}

esp_err_t cap_mcp_list_configured_servers(char *output, size_t output_size)
{
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    output[0] = '\0';

    cJSON *servers = NULL;
    esp_err_t err = cap_mcp_registry_list(&servers);
    if (err != ESP_OK) {
        return err;
    }

    size_t offset = 0;
    int count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, servers) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "id"));
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(item, "name"));
        const char *server_url = cJSON_GetStringValue(cJSON_GetObjectItem(item, "server_url"));
        const char *auth_type = cJSON_GetStringValue(cJSON_GetObjectItem(item, "auth_type"));
        cJSON *enabled_item = cJSON_GetObjectItem(item, "enabled");
        bool enabled = cJSON_IsBool(enabled_item) && cJSON_IsTrue(enabled_item);

        int written = snprintf(output + offset,
                               output_size - offset,
                               "- id=%s name=%s url=%s auth=%s enabled=%s\n",
                               id ? id : "?",
                               name ? name : "",
                               server_url ? server_url : "",
                               auth_type ? auth_type : "none",
                               enabled ? "true" : "false");
        if (written < 0 || (size_t)written >= output_size - offset) {
            offset = output_size - 1;
            break;
        }
        offset += (size_t)written;
        count++;
    }

    cJSON_Delete(servers);
    if (count == 0) {
        snprintf(output, output_size, "(no mcp servers configured)");
    }
    return ESP_OK;
}
