/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdbool.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"

#include "cap_mcp_client.h"
#include "cap_mcp_menu.h"
#include "cap_mcp_client_priv.h"

static esp_err_t cap_mcp_client_group_init(void)
{
    return ESP_OK;
}

static void cap_mcp_extract_content_text(const cJSON *content,
                                         char *output,
                                         size_t output_size)
{
    const cJSON *item = NULL;
    size_t offset = 0;

    if (!cJSON_IsArray(content) || output_size == 0) {
        if (output_size > 0) {
            output[0] = '\0';
        }
        return;
    }

    cJSON_ArrayForEach(item, content) {
        cJSON *type = cJSON_GetObjectItem(item, "type");

        if (!cJSON_IsString(type)) {
            continue;
        }

        if (strcmp(type->valuestring, "text") == 0) {
            cJSON *text = cJSON_GetObjectItem(item, "text");
            if (cJSON_IsString(text) && text->valuestring) {
                size_t len = strlen(text->valuestring);
                size_t room = output_size - 1 - offset;

                if (room > 0) {
                    if (len > room) {
                        len = room;
                    }
                    memcpy(output + offset, text->valuestring, len);
                    offset += len;
                }
            }
        }

        if (offset >= output_size - 1) {
            break;
        }
    }

    output[offset] = '\0';
}

/* cap_mcp_call_remote_tool / cap_mcp_list_remote_tools return one of these esp_err_t
 * values before ever reaching the server, for malformed or incomplete arguments. Their
 * only signal to the caller was previously esp_err_to_name() (e.g. "ESP_ERR_INVALID_ARG"),
 * which tells the model nothing about what to fix -- give it the actual missing piece so
 * it can self-correct instead of giving up. */
static void cap_mcp_request_error_message(esp_err_t err, char *output, size_t output_size)
{
    switch (err) {
    case ESP_ERR_INVALID_ARG:
        snprintf(output, output_size,
                 "Error: missing or invalid arguments. You must pass either 'server_id' "
                 "(the id of a server saved in the device's MCP settings -- call "
                 "mcp_list_servers to see valid ids) or an ad hoc 'server_url'. If you are "
                 "retrying a call that used server_id earlier in this conversation, pass "
                 "that same server_id again -- it is not carried over automatically.");
        break;
    case ESP_ERR_NOT_FOUND:
        snprintf(output, output_size,
                 "Error: that server_id is not in the device's MCP server registry. Call "
                 "mcp_list_servers to see valid ids.");
        break;
    case ESP_ERR_INVALID_STATE:
        snprintf(output, output_size,
                 "Error: that server_id is saved but disabled in the device's MCP "
                 "settings. Use a different server_id, or ask the user to enable it.");
        break;
    default:
        snprintf(output, output_size, "Error: MCP request failed (%s)", esp_err_to_name(err));
        break;
    }
}

/* Gateway-style MCP servers (e.g. Composio) route hundreds of per-app actions behind a
 * small set of meta-tools instead of exposing each action as its own top-level tool, so a
 * direct call with tool_name set to an action slug fails with a "not found" style message
 * -- either as a JSON-RPC protocol error, or (as Composio does) as a normal tool result
 * with isError=true and the message in content. Fold the fix-it steps into whichever form
 * of "not found" the model sees so it can self-correct in the same turn instead of
 * depending on it having pre-emptively activated a skill that documents this. */
static void cap_mcp_append_gateway_hint(char *output, size_t output_size)
{
    size_t len = strlen(output);

    if (len >= output_size - 1) {
        return;
    }
    snprintf(output + len, output_size - len,
             " -- This server may be a gateway that does not expose that name as a "
             "top-level tool directly (common for gateways like Composio that front many "
             "per-app actions behind a few meta-tools). Call mcp_list_tools on this same "
             "server_id to see the real top-level tool names, use its discovery/search tool "
             "to find the correct action slug, then call the execute/run meta-tool with that "
             "slug passed inside its arguments -- do not call the action slug directly as "
             "tool_name.");
}

/* Gateway servers with several accounts connected for one app (e.g. two Gmail logins) reject
 * a call that does not say which one to use, and list only opaque account ids. Models then tend
 * to guess an id and label the result with whatever address the user mentioned. Point them at
 * the step that maps ids to real emails, as part of the error they just received. The error can
 * arrive as a normal tool result rather than isError=true, so match on the text alone. */
#define CAP_MCP_MULTI_ACCOUNT_MARKER "accounts connected"

static void cap_mcp_append_multi_account_hint(char *output, size_t output_size)
{
    size_t len = strlen(output);

    if (len >= output_size - 1) {
        return;
    }
    snprintf(output + len, output_size - len,
             " -- To choose an account: call the gateway's connections meta-tool "
             "(MANAGE_CONNECTIONS) with action \"list\" for that toolkit to see each account's "
             "real email (for Gmail, GMAIL_GET_PROFILE with \"account\" returns emailAddress), pick the one the user means (ask if unclear), then repeat the call "
             "with \"account\" set to that id next to tool_slug. In your answer, state the exact "
             "email of the account you used; if the user named an address that is not in the "
             "list, say it is not connected instead of presenting another account as it.");
}

static esp_err_t cap_mcp_call_execute(const char *input_json,
                                      const claw_cap_call_context_t *ctx,
                                      char *output,
                                      size_t output_size)
{
    cJSON *result = NULL;
    const char *error_message = NULL;
    cJSON *is_error = NULL;
    bool failed = false;
    esp_err_t err;

    (void)ctx;

    err = cap_mcp_call_remote_tool(input_json, &result);
    if (err != ESP_OK) {
        cap_mcp_request_error_message(err, output, output_size);
        return err;
    }

    error_message = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error_message"));
    if (error_message && error_message[0]) {
        snprintf(output, output_size, "Error: %s", error_message);
        failed = true;
    } else {
        cap_mcp_extract_content_text(cJSON_GetObjectItem(result, "content"), output, output_size);
        is_error = cJSON_GetObjectItem(result, "isError");
        failed = cJSON_IsBool(is_error) && cJSON_IsTrue(is_error);
        if (output[0] == '\0') {
            if (failed) {
                snprintf(output, output_size, "Error: Tool returned application error");
            } else {
                snprintf(output, output_size, "(empty)");
            }
        }
    }

    cJSON_Delete(result);

    if (failed && strstr(output, "not found")) {
        cap_mcp_append_gateway_hint(output, output_size);
    }
    if (strstr(output, CAP_MCP_MULTI_ACCOUNT_MARKER)) {
        cap_mcp_append_multi_account_hint(output, output_size);
    }

    return ESP_OK;
}

static esp_err_t cap_mcp_list_execute(const char *input_json,
                                      const claw_cap_call_context_t *ctx,
                                      char *output,
                                      size_t output_size)
{
    cJSON *result = NULL;
    const char *error_message = NULL;
    cJSON *tools_array = NULL;
    cJSON *tool = NULL;
    size_t offset = 0;
    esp_err_t err;

    (void)ctx;

    err = cap_mcp_list_remote_tools(input_json, &result);
    if (err != ESP_OK) {
        cap_mcp_request_error_message(err, output, output_size);
        return err;
    }

    error_message = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error_message"));
    if (error_message && error_message[0]) {
        snprintf(output, output_size, "Error: %s", error_message);
        cJSON_Delete(result);
        return ESP_OK;
    }

    tools_array = cJSON_GetObjectItem(result, "tools");
    if (cJSON_IsArray(tools_array)) {
        cJSON_ArrayForEach(tool, tools_array) {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(tool, "name"));
            const char *description = cJSON_GetStringValue(cJSON_GetObjectItem(tool, "description"));
            int written = snprintf(output + offset,
                                   output_size - offset,
                                   "- %s: %s\n",
                                   name ? name : "(no name)",
                                   description ? description : "");

            if (written < 0 || (size_t)written >= output_size - offset) {
                offset = output_size - 1;
                break;
            }
            offset += (size_t)written;
        }
    }

    cJSON *next_cursor = cJSON_GetObjectItem(result, "nextCursor");
    if (cJSON_IsString(next_cursor) && next_cursor->valuestring[0] && offset < output_size - 1) {
        offset += snprintf(output + offset,
                           output_size - offset,
                           "\n(nextCursor: %s)",
                           next_cursor->valuestring);
    }
    if (offset == 0) {
        snprintf(output, output_size, "(no tools)");
    } else if (offset >= output_size) {
        output[output_size - 1] = '\0';
    }

    cJSON_Delete(result);
    return ESP_OK;
}

static esp_err_t cap_mcp_list_servers_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size)
{
    (void)input_json;
    (void)ctx;

    esp_err_t err = cap_mcp_list_configured_servers(output, output_size);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to read MCP server registry (%s)", esp_err_to_name(err));
    }
    return ESP_OK;
}

static esp_err_t cap_mcp_discover_execute(const char *input_json,
                                          const claw_cap_call_context_t *ctx,
                                          char *output,
                                          size_t output_size)
{
    cJSON *root = NULL;
    cJSON *devices = NULL;
    cJSON *device = NULL;
    size_t offset = 0;
    size_t found = 0;
    esp_err_t err;

    (void)ctx;

    err = cap_mcp_discover_services(input_json, &root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: mDNS MCP discovery failed (%s)", esp_err_to_name(err));
        return err;
    }

    devices = cJSON_GetObjectItem(root, "devices");
    if (cJSON_IsArray(devices)) {
        cJSON_ArrayForEach(device, devices) {
            const char *instance = cJSON_GetStringValue(cJSON_GetObjectItem(device, "instance"));
            const char *hostname = cJSON_GetStringValue(cJSON_GetObjectItem(device, "hostname"));
            const char *ip = cJSON_GetStringValue(cJSON_GetObjectItem(device, "ip"));
            const char *endpoint = cJSON_GetStringValue(cJSON_GetObjectItem(device, "endpoint"));
            const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(device, "url"));
            cJSON *port = cJSON_GetObjectItem(device, "port");
            int written = snprintf(output + offset,
                                   output_size - offset,
                                   "instance=%s\nhostname=%s\nip=%s\nport=%u\nendpoint=%s\nurl=%s\n\n",
                                   instance ? instance : "(unknown)",
                                   hostname ? hostname : "(unknown)",
                                   ip ? ip : "(unresolved)",
                                   cJSON_IsNumber(port) ? (unsigned)port->valueint : 0,
                                   endpoint ? endpoint : MCP_MDNS_DEFAULT_ENDPOINT,
                                   url ? url : "(unknown)");

            if (written < 0 || (size_t)written >= output_size - offset) {
                offset = output_size - 1;
                break;
            }
            offset += (size_t)written;
            found++;
        }
    }

    cJSON_Delete(root);
    if (found == 0) {
        snprintf(output, output_size, "(no mcp servers discovered)");
    } else if (offset >= 2) {
        output[offset - 1] = '\0';
    }

    return ESP_OK;
}

static const claw_cap_descriptor_t s_mcp_client_descriptors[] = {
    {
        .id = "mcp_list_tools",
        .name = "mcp_list_tools",
        .family = "mcp",
        .description = "List tools from a remote MCP server. Pass either server_id (a server "
                        "saved in the device's MCP settings, see mcp_list_servers) or an ad hoc "
                        "server_url.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"server_id\":{\"type\":\"string\"},\"server_url\":{\"type\":\"string\"},\"endpoint\":{\"type\":\"string\"},\"cursor\":{\"type\":\"string\"}}}",
        .execute = cap_mcp_list_execute,
    },
    {
        .id = "mcp_call_tool",
        .name = "mcp_call_tool",
        .family = "mcp",
        .description = "Call a tool on a remote MCP server. Pass either server_id (a server "
                        "saved in the device's MCP settings, see mcp_list_servers) or an ad hoc "
                        "server_url.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"server_id\":{\"type\":\"string\"},\"server_url\":{\"type\":\"string\"},\"endpoint\":{\"type\":\"string\"},\"tool_name\":{\"type\":\"string\"},\"arguments\":{\"type\":\"object\"}},\"required\":[\"tool_name\"]}",
        .execute = cap_mcp_call_execute,
    },
    {
        .id = "mcp_list_servers",
        .name = "mcp_list_servers",
        .family = "mcp",
        .description = "List MCP servers saved in the device's MCP settings (id, name, url, "
                        "auth type, enabled). Use a listed id as server_id in mcp_list_tools / "
                        "mcp_call_tool.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_mcp_list_servers_execute,
    },
    {
        .id = "mcp_discover",
        .name = "mcp_discover",
        .family = "mcp",
        .description = "Discover MCP servers advertised on the local network.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"timeout_ms\":{\"type\":\"integer\"},\"include_self\":{\"type\":\"boolean\"}}}",
        .execute = cap_mcp_discover_execute,
    },
};

static const claw_cap_group_t s_mcp_client_group = {
    .group_id = "cap_mcp_client",
    .descriptors = s_mcp_client_descriptors,
    .descriptor_count = sizeof(s_mcp_client_descriptors) / sizeof(s_mcp_client_descriptors[0]),
    .group_init = cap_mcp_client_group_init,
};

esp_err_t cap_mcp_client_register_group(void)
{
    esp_err_t err;

    if (claw_cap_group_exists(s_mcp_client_group.group_id)) {
        return ESP_OK;
    }

    err = claw_cap_register_group(&s_mcp_client_group);
    if (err != ESP_OK) {
        return err;
    }
    return cap_mcp_menu_register_group();
}

esp_err_t cap_mcp_client_test_server(const char *server_id, char *message, size_t message_size)
{
    if (!server_id || !server_id[0] || !message || message_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    message[0] = '\0';

    cJSON *input = cJSON_CreateObject();
    if (!input) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(input, "server_id", server_id);
    char *input_json = cJSON_PrintUnformatted(input);
    cJSON_Delete(input);
    if (!input_json) {
        return ESP_ERR_NO_MEM;
    }

    cJSON *result = NULL;
    esp_err_t err = cap_mcp_list_remote_tools(input_json, &result);
    cJSON_free(input_json);
    if (err != ESP_OK) {
        snprintf(message, message_size, "Error: %s", esp_err_to_name(err));
        return err;
    }

    const char *error_message = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error_message"));
    if (error_message && error_message[0]) {
        snprintf(message, message_size, "Error: %s", error_message);
    } else {
        cJSON *tools = cJSON_GetObjectItem(result, "tools");
        int count = cJSON_IsArray(tools) ? cJSON_GetArraySize(tools) : 0;
        snprintf(message, message_size, "OK: %d tool(s) found", count);
    }

    cJSON_Delete(result);
    return ESP_OK;
}
