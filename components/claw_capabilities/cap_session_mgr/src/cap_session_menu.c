/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* /session as a button menu. It runs the plain `session_command` capability,
 * then replies with the result plus the fresh session list, one button per
 * session. Button data are ordinary /session commands, so a tap behaves like
 * typing it. Non-Telegram channels get the same text without buttons. */
#include "cap_session_menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "cap_session_menu";

#define SESSION_MENU_LIST_BUF_LEN  768
#define SESSION_MENU_TEXT_LEN      1024
#define SESSION_MENU_NAME_LEN      40
#define SESSION_MENU_DATA_LEN      80
#define SESSION_MENU_ID_LEN        48
#define SESSION_MENU_CURRENT_TAG   " (current)"
#define SESSION_MENU_CHANNEL_TG    "telegram"
#define SESSION_MENU_LIST_PREFIX   "Sessions:"

typedef struct {
    const char *channel;
    const char *chat_id;
    const claw_cap_call_context_t *ctx;
} session_menu_target_t;

static const char *session_menu_text_send_cap(const char *channel)
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

/* Takes ownership of rows. */
static esp_err_t session_menu_reply(const session_menu_target_t *target, const char *text, cJSON *rows)
{
    cJSON *body = cJSON_CreateObject();
    char *body_json;
    char output[96];
    const char *send_cap;
    bool telegram = strcmp(target->channel, SESSION_MENU_CHANNEL_TG) == 0;
    esp_err_t err;

    if (!body) {
        cJSON_Delete(rows);
        return ESP_ERR_NO_MEM;
    }
    send_cap = telegram ? "tg_send_menu" : session_menu_text_send_cap(target->channel);
    cJSON_AddStringToObject(body, "chat_id", target->chat_id);
    cJSON_AddStringToObject(body, telegram ? "text" : "message", text);
    if (telegram && rows) {
        cJSON_AddItemToObject(body, "buttons", rows);
        rows = NULL;
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

static void session_menu_button(cJSON *row, const char *label, const char *command, const char *arg)
{
    cJSON *button = cJSON_CreateObject();
    char data[SESSION_MENU_DATA_LEN];

    if (!row || !button) {
        cJSON_Delete(button);
        return;
    }
    snprintf(data, sizeof(data), "%s%s%s", command, arg ? " " : "", arg ? arg : "");
    cJSON_AddStringToObject(button, "text", label);
    cJSON_AddStringToObject(button, "data", data);
    cJSON_AddItemToArray(row, button);
}

static cJSON *session_menu_row(cJSON *rows)
{
    cJSON *row = cJSON_CreateArray();

    if (row) {
        cJSON_AddItemToArray(rows, row);
    }
    return row;
}

/* Parses "Sessions:\n* name[ (current)]\n..." into one button row per session. */
static void session_menu_add_session_rows(cJSON *rows, const char *list_output)
{
    const char *line = strchr(list_output, '\n');

    while (line && *line) {
        const char *start = line + 1;
        const char *eol = strchr(start, '\n');
        size_t len = eol ? (size_t)(eol - start) : strlen(start);

        if (len > 2 && start[0] == '*' && start[1] == ' ') {
            char name[SESSION_MENU_NAME_LEN];
            char label[SESSION_MENU_NAME_LEN + 16];
            size_t tag_len = strlen(SESSION_MENU_CURRENT_TAG);
            bool current = len > tag_len + 2 && strncmp(start + len - tag_len, SESSION_MENU_CURRENT_TAG, tag_len) == 0;
            size_t name_len = len - 2 - (current ? tag_len : 0);
            cJSON *row = session_menu_row(rows);

            if (name_len >= sizeof(name)) {
                name_len = sizeof(name) - 1;
            }
            memcpy(name, start + 2, name_len);
            name[name_len] = '\0';
            if (current) {
                snprintf(label, sizeof(label), "\xe2\x9c\x85 %s", name);
                session_menu_button(row, label, "/session list", NULL);
            } else {
                snprintf(label, sizeof(label), "\xe2\x96\xb6\xef\xb8\x8f %s", name);
                session_menu_button(row, label, "/session switch", name);
                session_menu_button(row, "\xf0\x9f\x97\x91", "/session ask-delete", name);
            }
        }
        line = eol;
    }
}

static esp_err_t session_menu_send_list(const session_menu_target_t *target, const char *header)
{
    char *list = calloc(1, SESSION_MENU_LIST_BUF_LEN);
    char *text = calloc(1, SESSION_MENU_TEXT_LEN);
    cJSON *rows = cJSON_CreateArray();
    cJSON *row;
    esp_err_t err = ESP_ERR_NO_MEM;

    if (!list || !text || !rows) {
        goto done;
    }
    err = claw_cap_call("session_command", "{\"command\":\"list\"}", target->ctx, list, SESSION_MENU_LIST_BUF_LEN);
    if (err != ESP_OK || strncmp(list, SESSION_MENU_LIST_PREFIX, strlen(SESSION_MENU_LIST_PREFIX)) != 0) {
        /* Not a session list (e.g. storage error): show what the command said. */
        cJSON_Delete(rows);
        err = session_menu_reply(target, list[0] ? list : "Sessions are not available right now.", NULL);
        goto done_no_rows;
    }
    snprintf(text, SESSION_MENU_TEXT_LEN, "%s%s\xf0\x9f\x92\xac Sessions - tap one to switch\n%s",
             header ? header : "", header ? "\n\n" : "", strchr(list, '\n') ? strchr(list, '\n') : "");
    session_menu_add_session_rows(rows, list);
    row = session_menu_row(rows);
    session_menu_button(row, "\xf0\x9f\x86\x95 New session", "/session new", NULL);
    session_menu_button(row, "\xf0\x9f\x94\x84 Refresh", "/session list", NULL);
    err = session_menu_reply(target, text, rows);
    goto done_no_rows;

done:
    cJSON_Delete(rows);
done_no_rows:
    free(list);
    free(text);
    return err;
}

static esp_err_t session_menu_confirm_delete(const session_menu_target_t *target, const char *name)
{
    cJSON *rows = cJSON_CreateArray();
    cJSON *row = rows ? session_menu_row(rows) : NULL;
    char text[SESSION_MENU_TEXT_LEN];

    snprintf(text, sizeof(text), "\xf0\x9f\x97\x91 Delete session \"%s\"? Its history will be lost.", name);
    session_menu_button(row, "\xf0\x9f\x97\x91 Yes, delete", "/session delete", name);
    session_menu_button(row, "Cancel", "/session list", NULL);
    return session_menu_reply(target, text, rows);
}

static esp_err_t session_menu_dispatch(const session_menu_target_t *target, const char *command)
{
    static const char ask_delete[] = "ask-delete ";
    char *result;
    cJSON *root;
    char *input_json;
    esp_err_t err;

    while (*command == ' ') {
        command++;
    }
    if (strncmp(command, ask_delete, strlen(ask_delete)) == 0) {
        return session_menu_confirm_delete(target, command + strlen(ask_delete));
    }
    if (!command[0] || strcmp(command, "list") == 0 || strcmp(command, "menu") == 0) {
        return session_menu_send_list(target, NULL);
    }

    result = calloc(1, SESSION_MENU_TEXT_LEN);
    root = cJSON_CreateObject();
    if (!result || !root) {
        free(result);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "command", command);
    input_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!input_json) {
        free(result);
        return ESP_ERR_NO_MEM;
    }
    err = claw_cap_call("session_command", input_json, target->ctx, result, SESSION_MENU_TEXT_LEN);
    free(input_json);
    if (err == ESP_OK) {
        err = session_menu_send_list(target, result);
    }
    free(result);
    return err;
}

esp_err_t cap_session_menu_execute(const char *input_json,
                                   const claw_cap_call_context_t *ctx,
                                   char *output,
                                   size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    const cJSON *command = root ? cJSON_GetObjectItem(root, "command") : NULL;
    const cJSON *channel = root ? cJSON_GetObjectItem(root, "channel") : NULL;
    const cJSON *chat_id = root ? cJSON_GetObjectItem(root, "chat_id") : NULL;
    session_menu_target_t target = {0};
    char channel_copy[SESSION_MENU_ID_LEN];
    char chat_copy[SESSION_MENU_ID_LEN];
    char *command_copy;
    esp_err_t err;

    if (!cJSON_IsString(channel) || !cJSON_IsString(chat_id)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: channel and chat_id are required");
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(channel_copy, channel->valuestring, sizeof(channel_copy));
    strlcpy(chat_copy, chat_id->valuestring, sizeof(chat_copy));
    command_copy = strdup(cJSON_IsString(command) ? command->valuestring : "");
    cJSON_Delete(root);
    if (!command_copy) {
        return ESP_ERR_NO_MEM;
    }
    target.channel = channel_copy;
    target.chat_id = chat_copy;
    target.ctx = ctx;
    err = session_menu_dispatch(&target, command_copy);
    free(command_copy);
    snprintf(output, output_size, "%s", err == ESP_OK ? "session menu sent" : esp_err_to_name(err));
    return err;
}
