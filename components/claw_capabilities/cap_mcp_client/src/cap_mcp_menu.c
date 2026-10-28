/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* Interactive /mcp chat command: list, add, test, inspect, enable/disable and
 * delete saved MCP servers. On Telegram every screen carries inline buttons
 * whose callback data is another /mcp command, so a tap behaves exactly like
 * typing it. Other channels get the same text without buttons. */
#include "cap_mcp_menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cap_mcp_client.h"
#include "cap_mcp_registry.h"
#include "claw_cap.h"
#include "claw_task.h"
#include "claw_utils_string.h"
#include "esp_log.h"

static const char *TAG = "cap_mcp_menu";

#define MCP_MENU_CMD_LEN        96
#define MCP_MENU_TEXT_LEN       1024
#define MCP_MENU_TOOLS_BUF_LEN  16384
#define MCP_MENU_TOOLS_TEXT_LEN 3500
#define MCP_MENU_TOOL_DESC_LEN  70
#define MCP_MENU_SERVER_TOOL_NAME_MAX 64
#define MCP_MENU_ENDPOINT       "mcp"
#define MCP_MENU_TASK_STACK     10240
#define MCP_MENU_TASK_PRIORITY  5
#define MCP_MENU_ID_LEN         48
#define MCP_MENU_CHANNEL_TG     "telegram"

#define MCP_CMD_MENU    "/mcp"
#define MCP_CMD_LIST    "/mcp list"
#define MCP_CMD_ADD     "/mcp add"
#define MCP_CMD_SHOW    "/mcp show"
#define MCP_CMD_TEST    "/mcp test"
#define MCP_CMD_TOOLS   "/mcp tools"
#define MCP_CMD_ON      "/mcp on"
#define MCP_CMD_OFF     "/mcp off"
#define MCP_CMD_DELETE  "/mcp delete"
#define MCP_CMD_REMOVE  "/mcp remove"

typedef struct {
    const char *channel;
    const char *chat_id;
    const claw_cap_call_context_t *ctx;
} mcp_menu_target_t;

static const char *mcp_menu_text_send_cap(const char *channel)
{
    static const struct {
        const char *channel;
        const char *cap;
    } map[] = {
        { "feishu", "feishu_send_message" },
        { "qq", "qq_send_message" },
        { "wechat", "wechat_send_message" },
        { "web", "local_send_message" },
    };

    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(channel, map[i].channel) == 0) {
            return map[i].cap;
        }
    }
    return NULL;
}

/* Sends text (and buttons on Telegram) to the chat. Takes ownership of rows. */
static esp_err_t mcp_menu_reply(const mcp_menu_target_t *target, const char *text, cJSON *rows)
{
    cJSON *body = cJSON_CreateObject();
    char *body_json;
    char output[96];
    const char *send_cap;
    esp_err_t err;

    if (!body) {
        cJSON_Delete(rows);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(body, "chat_id", target->chat_id);
    if (strcmp(target->channel, MCP_MENU_CHANNEL_TG) == 0) {
        send_cap = "tg_send_menu";
        cJSON_AddStringToObject(body, "text", text);
        if (rows) {
            cJSON_AddItemToObject(body, "buttons", rows);
            rows = NULL;
        }
    } else {
        send_cap = mcp_menu_text_send_cap(target->channel);
        cJSON_AddStringToObject(body, "message", text);
    }
    cJSON_Delete(rows);
    if (!send_cap) {
        cJSON_Delete(body);
        return ESP_ERR_NOT_SUPPORTED;
    }
    body_json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_json) {
        return ESP_ERR_NO_MEM;
    }
    err = claw_cap_call(send_cap, body_json, target->ctx, output, sizeof(output));
    free(body_json);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "send via %s failed: %s", send_cap, esp_err_to_name(err));
    }
    return err;
}

static cJSON *mcp_menu_new_rows(void)
{
    return cJSON_CreateArray();
}

static cJSON *mcp_menu_row(cJSON *rows)
{
    cJSON *row = cJSON_CreateArray();

    if (row && rows) {
        cJSON_AddItemToArray(rows, row);
    }
    return row;
}

/* Adds a button whose tap sends "<command>[ <arg>]" back as a chat message. */
static void mcp_menu_button(cJSON *row, const char *label, const char *command, const char *arg)
{
    cJSON *button = cJSON_CreateObject();
    char data[MCP_MENU_CMD_LEN];

    if (!row || !button) {
        cJSON_Delete(button);
        return;
    }
    snprintf(data, sizeof(data), "%s%s%s", command, arg ? " " : "", arg ? arg : "");
    cJSON_AddStringToObject(button, "text", label);
    cJSON_AddStringToObject(button, "data", data);
    cJSON_AddItemToArray(row, button);
}

static void mcp_menu_nav_row(cJSON *rows, const char *show_id)
{
    cJSON *row = mcp_menu_row(rows);

    if (show_id) {
        mcp_menu_button(row, "\xe2\xac\x85\xef\xb8\x8f Back", MCP_CMD_SHOW, show_id);
    }
    mcp_menu_button(row, "\xf0\x9f\x93\x8b Servers", MCP_CMD_LIST, NULL);
}

static esp_err_t mcp_menu_main(const mcp_menu_target_t *target)
{
    cJSON *servers = NULL;
    cJSON *rows = mcp_menu_new_rows();
    cJSON *row = mcp_menu_row(rows);
    char text[MCP_MENU_TEXT_LEN];
    int total = 0;

    if (cap_mcp_registry_list(&servers) == ESP_OK) {
        total = cJSON_GetArraySize(servers);
    }
    cJSON_Delete(servers);
    snprintf(text, sizeof(text), "\xf0\x9f\x94\x8c MCP servers\n\n%d saved. MCP servers give the assistant "
             "extra tools (email, calendar, files...). What do you want to do?", total);
    mcp_menu_button(row, "\xf0\x9f\x93\x8b Servers", MCP_CMD_LIST, NULL);
    mcp_menu_button(row, "\xe2\x9e\x95 Add server", MCP_CMD_ADD, NULL);
    return mcp_menu_reply(target, text, rows);
}

static esp_err_t mcp_menu_list(const mcp_menu_target_t *target)
{
    cJSON *servers = NULL;
    cJSON *rows = mcp_menu_new_rows();
    cJSON *item = NULL;
    const char *text = "Tap a server to manage it.";

    if (cap_mcp_registry_list(&servers) != ESP_OK || cJSON_GetArraySize(servers) == 0) {
        text = "No MCP servers saved yet.";
    }
    cJSON_ArrayForEach(item, servers) {
        const cJSON *id = cJSON_GetObjectItem(item, "id");
        const cJSON *name = cJSON_GetObjectItem(item, "name");
        const cJSON *enabled = cJSON_GetObjectItem(item, "enabled");
        char label[MCP_MENU_CMD_LEN];

        if (!cJSON_IsString(id)) {
            continue;
        }
        snprintf(label, sizeof(label), "%s %s", cJSON_IsFalse(enabled) ? "\xe2\x8f\xb8" : "\xe2\x9c\x85",
                 cJSON_IsString(name) && name->valuestring[0] ? name->valuestring : id->valuestring);
        mcp_menu_button(mcp_menu_row(rows), label, MCP_CMD_SHOW, id->valuestring);
    }
    cJSON_Delete(servers);
    mcp_menu_button(mcp_menu_row(rows), "\xe2\x9e\x95 Add server", MCP_CMD_ADD, NULL);
    return mcp_menu_reply(target, text, rows);
}

static esp_err_t mcp_menu_add_help(const mcp_menu_target_t *target)
{
    cJSON *rows = mcp_menu_new_rows();

    mcp_menu_button(mcp_menu_row(rows), "\xe2\xac\x85\xef\xb8\x8f Back", MCP_CMD_MENU, NULL);
    return mcp_menu_reply(target,
                          "\xe2\x9e\x95 Add an MCP server\n\n"
                          "Send:\n/mcp add <name> <url> [token]\n\n"
                          "Example:\n/mcp add composio https://connect.composio.dev/mcp YOUR_API_KEY\n\n"
                          "The name is one word. The token is optional and is sent as a Bearer token. "
                          "I will test the connection right after saving.\n\n"
                          "\xe2\x9a\xa0\xef\xb8\x8f Your message contains the token: delete it from the chat afterwards.",
                          rows);
}

static esp_err_t mcp_menu_not_found(const mcp_menu_target_t *target, const char *id)
{
    char text[MCP_MENU_TEXT_LEN];
    cJSON *rows = mcp_menu_new_rows();

    snprintf(text, sizeof(text), "I can't find a server with id \"%s\".", id);
    mcp_menu_nav_row(rows, NULL);
    return mcp_menu_reply(target, text, rows);
}

static esp_err_t mcp_menu_show(const mcp_menu_target_t *target, const char *id)
{
    cap_mcp_server_entry_t *entry = calloc(1, sizeof(*entry));
    cJSON *rows = mcp_menu_new_rows();
    cJSON *row;
    char text[MCP_MENU_TEXT_LEN];
    esp_err_t err;

    if (!entry) {
        cJSON_Delete(rows);
        return ESP_ERR_NO_MEM;
    }
    if (cap_mcp_registry_get(id, entry) != ESP_OK) {
        free(entry);
        cJSON_Delete(rows);
        return mcp_menu_not_found(target, id);
    }
    snprintf(text, sizeof(text), "%s %s\n\nURL: %s\nAuth: %s\nStatus: %s\nId: %s",
             entry->enabled ? "\xe2\x9c\x85" : "\xe2\x8f\xb8", entry->name[0] ? entry->name : entry->id,
             entry->server_url, cap_mcp_registry_auth_type_to_str(entry->auth_type),
             entry->enabled ? "enabled" : "disabled", entry->id);
    row = mcp_menu_row(rows);
    mcp_menu_button(row, "\xf0\x9f\x94\x8d Test", MCP_CMD_TEST, id);
    mcp_menu_button(row, "\xf0\x9f\xa7\xb0 Tools", MCP_CMD_TOOLS, id);
    row = mcp_menu_row(rows);
    mcp_menu_button(row, entry->enabled ? "\xe2\x8f\xb8 Disable" : "\xe2\x96\xb6\xef\xb8\x8f Enable",
                    entry->enabled ? MCP_CMD_OFF : MCP_CMD_ON, id);
    mcp_menu_button(row, "\xf0\x9f\x97\x91 Delete", MCP_CMD_DELETE, id);
    mcp_menu_nav_row(rows, NULL);
    free(entry);
    err = mcp_menu_reply(target, text, rows);
    return err;
}

static esp_err_t mcp_menu_test(const mcp_menu_target_t *target, const char *id)
{
    char message[256] = {0};
    char text[MCP_MENU_TEXT_LEN];
    cJSON *rows = mcp_menu_new_rows();
    cJSON *row = mcp_menu_row(rows);
    esp_err_t err = cap_mcp_client_test_server(id, message, sizeof(message));

    snprintf(text, sizeof(text), "%s %s", err == ESP_OK && strncmp(message, "OK", 2) == 0 ? "\xe2\x9c\x85" : "\xe2\x9d\x8c",
             message[0] ? message : esp_err_to_name(err));
    mcp_menu_button(row, "\xf0\x9f\xa7\xb0 Tools", MCP_CMD_TOOLS, id);
    mcp_menu_nav_row(rows, id);
    return mcp_menu_reply(target, text, rows);
}

/* mcp_list_tools prints "- NAME: description" with arbitrarily long, multi-line
 * descriptions. Keep one short line per tool: the name and the start of its
 * first description line. */
static bool mcp_menu_is_tool_name(const char *name, size_t len)
{
    bool has_underscore = false;
    bool has_upper = false;

    if (len == 0 || len > MCP_MENU_SERVER_TOOL_NAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = name[i];

        has_underscore = has_underscore || c == '_';
        has_upper = has_upper || (c >= 'A' && c <= 'Z');
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '_' || c == '-' || c == '.')) {
            return false;
        }
    }
    /* Prose such as "Home" is capitalised without underscores; tool slugs are not. */
    return has_underscore || !has_upper;
}

static void mcp_menu_summarize_tools(const char *listing, char *out, size_t out_size)
{
    const char *line = listing;
    size_t used = 0;

    out[0] = '\0';
    while (*line && used + 1 < out_size) {
        const char *eol = strchr(line, '\n');
        size_t line_len = eol ? (size_t)(eol - line) : strlen(line);

        if (line_len > 2 && line[0] == '-' && line[1] == ' ') {
            const char *colon = memchr(line, ':', line_len);
            size_t name_len = colon ? (size_t)(colon - line) - 2 : line_len - 2;

            /* Bullet points inside a description also start with "- ": skip them. */
            if (!mcp_menu_is_tool_name(line + 2, name_len)) {
                line = eol ? eol + 1 : line + line_len;
                continue;
            }
            const char *desc = colon ? colon + 1 : line + line_len;
            size_t desc_len = (size_t)(line + line_len - desc);

            while (desc_len > 0 && *desc == ' ') {
                desc++;
                desc_len--;
            }
            if (desc_len > MCP_MENU_TOOL_DESC_LEN) {
                desc_len = claw_utils_utf8_prefix_len(desc, MCP_MENU_TOOL_DESC_LEN);
            }
            used += (size_t)snprintf(out + used, out_size - used, "\xe2\x80\xa2 %.*s%s%.*s\n",
                                     (int)name_len, line + 2, desc_len ? " - " : "", (int)desc_len, desc);
            if (used >= out_size) {
                used = out_size - 1;
            }
        }
        line = eol ? eol + 1 : line + line_len;
    }
}

static esp_err_t mcp_menu_tools(const mcp_menu_target_t *target, const char *id)
{
    char *output = calloc(1, MCP_MENU_TOOLS_BUF_LEN);
    char input[MCP_MENU_CMD_LEN + 32];
    cJSON *rows = mcp_menu_new_rows();
    char *text;
    char *summary;
    esp_err_t err;

    if (!output) {
        cJSON_Delete(rows);
        return ESP_ERR_NO_MEM;
    }
    snprintf(input, sizeof(input), "{\"server_id\":\"%s\"}", id);
    err = claw_cap_call("mcp_list_tools", input, target->ctx, output, MCP_MENU_TOOLS_BUF_LEN);
    text = calloc(1, MCP_MENU_TOOLS_TEXT_LEN + 64);
    summary = calloc(1, MCP_MENU_TOOLS_TEXT_LEN);
    if (!text || !summary) {
        free(text);
        free(summary);
        free(output);
        cJSON_Delete(rows);
        return ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        mcp_menu_summarize_tools(output, summary, MCP_MENU_TOOLS_TEXT_LEN);
    }
    snprintf(text, MCP_MENU_TOOLS_TEXT_LEN + 64, "\xf0\x9f\xa7\xb0 Tools (%s)\n\n%s", id,
             summary[0] ? summary : "Could not list tools.");
    mcp_menu_nav_row(rows, id);
    err = mcp_menu_reply(target, text, rows);
    free(text);
    free(summary);
    free(output);
    return err;
}

static esp_err_t mcp_menu_set_enabled(const mcp_menu_target_t *target, const char *id, bool enabled)
{
    cap_mcp_server_entry_t *entry = calloc(1, sizeof(*entry));
    esp_err_t err;

    if (!entry) {
        return ESP_ERR_NO_MEM;
    }
    if (cap_mcp_registry_get(id, entry) != ESP_OK) {
        free(entry);
        return mcp_menu_not_found(target, id);
    }
    entry->enabled = enabled;
    err = cap_mcp_registry_upsert(entry);
    free(entry);
    if (err != ESP_OK) {
        return mcp_menu_reply(target, "Could not save the change.", NULL);
    }
    return mcp_menu_show(target, id);
}

static esp_err_t mcp_menu_confirm_delete(const mcp_menu_target_t *target, const char *id)
{
    cJSON *rows = mcp_menu_new_rows();
    cJSON *row = mcp_menu_row(rows);
    char text[MCP_MENU_TEXT_LEN];

    snprintf(text, sizeof(text), "\xf0\x9f\x97\x91 Delete server \"%s\"? The assistant will lose its tools.", id);
    mcp_menu_button(row, "\xf0\x9f\x97\x91 Yes, delete", MCP_CMD_REMOVE, id);
    mcp_menu_button(row, "Cancel", MCP_CMD_SHOW, id);
    return mcp_menu_reply(target, text, rows);
}

static esp_err_t mcp_menu_remove(const mcp_menu_target_t *target, const char *id)
{
    if (cap_mcp_registry_delete(id) != ESP_OK) {
        return mcp_menu_not_found(target, id);
    }
    return mcp_menu_list(target);
}

/* "/mcp add <name> <url> [token]" */
static esp_err_t mcp_menu_add(const mcp_menu_target_t *target, char *args)
{
    cap_mcp_server_entry_t *entry = NULL;
    char *name = strtok_r(args, " \t", &args);
    char *url = strtok_r(NULL, " \t", &args);
    char *token = strtok_r(NULL, " \t", &args);
    esp_err_t err;

    if (!name || !url || strncmp(url, "http", 4) != 0) {
        return mcp_menu_add_help(target);
    }
    entry = calloc(1, sizeof(*entry));
    if (!entry) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(entry->name, name, sizeof(entry->name));
    strlcpy(entry->server_url, url, sizeof(entry->server_url));
    strlcpy(entry->endpoint, MCP_MENU_ENDPOINT, sizeof(entry->endpoint));
    entry->enabled = true;
    if (token) {
        entry->auth_type = CAP_MCP_AUTH_BEARER;
        strlcpy(entry->secret, token, sizeof(entry->secret));
    }
    err = cap_mcp_registry_upsert(entry);
    if (err != ESP_OK) {
        free(entry);
        return mcp_menu_reply(target, "Could not save the server.", NULL);
    }
    mcp_menu_reply(target, "\xe2\x9c\x85 Saved. Testing the connection...", NULL);
    err = mcp_menu_test(target, entry->id);
    free(entry);
    return err;
}

static char *mcp_menu_trim(char *text)
{
    char *end;

    while (*text == ' ' || *text == '\t') {
        text++;
    }
    end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) {
        *--end = '\0';
    }
    return text;
}

static esp_err_t mcp_menu_dispatch(const mcp_menu_target_t *target, char *command)
{
    char *args = command;
    char *sub = strtok_r(command, " \t", &args);
    char *arg = args ? mcp_menu_trim(args) : (char *)"";

    if (!sub || strcmp(sub, "menu") == 0 || strcmp(sub, "help") == 0) {
        return mcp_menu_main(target);
    }
    if (strcmp(sub, "list") == 0) {
        return mcp_menu_list(target);
    }
    if (strcmp(sub, "add") == 0) {
        return mcp_menu_add(target, arg);
    }
    if (!arg[0]) {
        return mcp_menu_main(target);
    }
    if (strcmp(sub, "show") == 0) {
        return mcp_menu_show(target, arg);
    }
    if (strcmp(sub, "test") == 0) {
        return mcp_menu_test(target, arg);
    }
    if (strcmp(sub, "tools") == 0) {
        return mcp_menu_tools(target, arg);
    }
    if (strcmp(sub, "on") == 0 || strcmp(sub, "off") == 0) {
        return mcp_menu_set_enabled(target, arg, strcmp(sub, "on") == 0);
    }
    if (strcmp(sub, "delete") == 0) {
        return mcp_menu_confirm_delete(target, arg);
    }
    if (strcmp(sub, "remove") == 0) {
        return mcp_menu_remove(target, arg);
    }
    return mcp_menu_main(target);
}

/* Menu actions do network I/O (TLS handshake to the MCP server) and need far
 * more stack than the event router task has, so each command runs on its own
 * short-lived task and the router is released immediately. */
typedef struct {
    char *command;
    char channel[MCP_MENU_ID_LEN];
    char chat_id[MCP_MENU_ID_LEN];
} mcp_menu_job_t;

static volatile bool s_mcp_menu_busy;

static void mcp_menu_task(void *arg)
{
    mcp_menu_job_t *job = (mcp_menu_job_t *)arg;
    claw_cap_call_context_t ctx = {
        .channel = job->channel,
        .chat_id = job->chat_id,
        .source_cap = "cap_mcp_menu",
        .caller = CLAW_CAP_CALLER_SYSTEM,
    };
    mcp_menu_target_t target = { .channel = job->channel, .chat_id = job->chat_id, .ctx = &ctx };

    mcp_menu_dispatch(&target, mcp_menu_trim(job->command));
    free(job->command);
    free(job);
    s_mcp_menu_busy = false;
    claw_task_delete(NULL);
}

static esp_err_t mcp_menu_execute(const char *input_json,
                                  const claw_cap_call_context_t *ctx,
                                  char *output,
                                  size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    const cJSON *command = root ? cJSON_GetObjectItem(root, "command") : NULL;
    const cJSON *channel = root ? cJSON_GetObjectItem(root, "channel") : NULL;
    const cJSON *chat_id = root ? cJSON_GetObjectItem(root, "chat_id") : NULL;
    mcp_menu_job_t *job = NULL;

    (void)ctx;
    if (!cJSON_IsString(channel) || !cJSON_IsString(chat_id)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: channel and chat_id are required");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mcp_menu_busy) {
        cJSON_Delete(root);
        snprintf(output, output_size, "mcp menu busy");
        return ESP_OK;
    }
    job = calloc(1, sizeof(*job));
    if (job) {
        job->command = strdup(cJSON_IsString(command) ? command->valuestring : "");
        strlcpy(job->channel, channel->valuestring, sizeof(job->channel));
        strlcpy(job->chat_id, chat_id->valuestring, sizeof(job->chat_id));
    }
    cJSON_Delete(root);
    if (!job || !job->command) {
        free(job);
        return ESP_ERR_NO_MEM;
    }

    s_mcp_menu_busy = true;
    if (claw_task_create(&(claw_task_config_t){
                             .name = "mcp_menu",
                             .stack_size = MCP_MENU_TASK_STACK,
                             .priority = MCP_MENU_TASK_PRIORITY,
                             .core_id = tskNO_AFFINITY,
                             .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
                         },
                         mcp_menu_task, job, NULL) != pdPASS) {
        s_mcp_menu_busy = false;
        free(job->command);
        free(job);
        return ESP_FAIL;
    }
    snprintf(output, output_size, "mcp menu started");
    return ESP_OK;
}

static const claw_cap_descriptor_t s_mcp_menu_descriptors[] = {
    {
        .id = "mcp_menu",
        .name = "mcp_menu",
        .family = "mcp",
        .description = "Interactive /mcp chat menu (list, add, test, tools, enable, delete MCP servers).",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = 0,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},\"channel\":{\"type\":\"string\"},\"chat_id\":{\"type\":\"string\"}},\"required\":[\"channel\",\"chat_id\"]}",
        .execute = mcp_menu_execute,
    },
};

static const claw_cap_group_t s_mcp_menu_group = {
    .group_id = "cap_mcp_menu",
    .descriptors = s_mcp_menu_descriptors,
    .descriptor_count = sizeof(s_mcp_menu_descriptors) / sizeof(s_mcp_menu_descriptors[0]),
};

esp_err_t cap_mcp_menu_register_group(void)
{
    if (claw_cap_group_exists(s_mcp_menu_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_mcp_menu_group);
}
