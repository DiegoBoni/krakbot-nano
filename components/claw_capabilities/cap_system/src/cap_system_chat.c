/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* Plain-text capabilities behind the chat slash commands (/help, /status,
 * /web, /tasks, /digest). Router rules send the reply text to the chat as-is.
 * Only status and web_url are also exposed to the LLM, so the user can simply
 * ask "what is the settings link?". */
#include "cap_system_chat.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"

static const char *TAG = "cap_system_chat";

#define CAP_CHAT_SCHEDULER_BUF_LEN  6144
#define CAP_CHAT_TIME_LEN           32
#define CAP_CHAT_HOSTNAME_LEN       64
#define CAP_CHAT_ID_LEN             64
#define CAP_CHAT_SECONDS_PER_MIN    60
#define CAP_CHAT_SECONDS_PER_HOUR   3600
#define CAP_CHAT_SECONDS_PER_DAY    86400
#define CAP_CHAT_MS_PER_SECOND      1000
#define CAP_CHAT_KIB                1024

static const char *const s_chat_help_text =
    "KrakBot Nano commands\n"
    "\n"
    "/new - start a fresh conversation\n"
    "/session - list or switch conversations\n"
    "/llm - show or change the AI model (/llm status)\n"
    "/mcp - manage MCP servers (add, test, tools)\n"
    "/cost - token usage and approximate cost\n"
    "/status - Wi-Fi, memory, uptime and clock\n"
    "/web - link to the settings page\n"
    "/tasks - scheduled jobs and next run\n"
    "/digest - send the daily summary now\n"
    "/help - this list\n"
    "\n"
    "Anything else you write goes to the assistant.";

static esp_err_t cap_chat_write(char *output, size_t output_size, const char *text, esp_err_t err)
{
    snprintf(output, output_size, "%s", text);
    return err;
}

static esp_err_t cap_chat_help_execute(const char *input_json,
                                       const claw_cap_call_context_t *ctx,
                                       char *output,
                                       size_t output_size)
{
    (void)input_json;
    (void)ctx;
    return cap_chat_write(output, output_size, s_chat_help_text, ESP_OK);
}

static bool cap_chat_get_sta_ip(char *ip_buf, size_t ip_buf_size)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {0};

    if (!netif || esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0) {
        return false;
    }
    snprintf(ip_buf, ip_buf_size, IPSTR, IP2STR(&info.ip));
    return true;
}

static esp_err_t cap_chat_web_execute(const char *input_json,
                                      const claw_cap_call_context_t *ctx,
                                      char *output,
                                      size_t output_size)
{
    char ip[16];
    char host[CAP_CHAT_HOSTNAME_LEN] = {0};
    size_t used;

    (void)input_json;
    (void)ctx;
    if (!cap_chat_get_sta_ip(ip, sizeof(ip))) {
        return cap_chat_write(output, output_size,
                              "The device has no Wi-Fi address right now.", ESP_OK);
    }
    used = (size_t)snprintf(output, output_size, "Settings page (same Wi-Fi network):\nhttp://%s/", ip);
    if (mdns_hostname_get(host) == ESP_OK && host[0] && used < output_size) {
        snprintf(output + used, output_size - used, "\nor http://%s.local/", host);
    }
    return ESP_OK;
}

static void cap_chat_format_uptime(char *buf, size_t size, int64_t seconds)
{
    int64_t days = seconds / CAP_CHAT_SECONDS_PER_DAY;
    int64_t hours = (seconds % CAP_CHAT_SECONDS_PER_DAY) / CAP_CHAT_SECONDS_PER_HOUR;
    int64_t minutes = (seconds % CAP_CHAT_SECONDS_PER_HOUR) / CAP_CHAT_SECONDS_PER_MIN;

    snprintf(buf, size, "%" PRId64 "d %" PRId64 "h %" PRId64 "m", days, hours, minutes);
}

static esp_err_t cap_chat_status_execute(const char *input_json,
                                         const claw_cap_call_context_t *ctx,
                                         char *output,
                                         size_t output_size)
{
    char ip[16] = "-";
    char uptime[CAP_CHAT_TIME_LEN];
    char now_text[CAP_CHAT_TIME_LEN] = "not synced";
    wifi_ap_record_t ap = {0};
    time_t now = time(NULL);
    struct tm tm_now = {0};
    size_t used;

    (void)input_json;
    (void)ctx;
    cap_chat_get_sta_ip(ip, sizeof(ip));
    cap_chat_format_uptime(uptime, sizeof(uptime), esp_timer_get_time() / 1000000);
    if (now > 1700000000) {
        localtime_r(&now, &tm_now);
        strftime(now_text, sizeof(now_text), "%Y-%m-%d %H:%M", &tm_now);
    }

    used = (size_t)snprintf(output, output_size, "KrakBot Nano %s\nUptime: %s\nClock: %s\n",
                            esp_app_get_description()->version, uptime, now_text);
    if (used < output_size && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        used += (size_t)snprintf(output + used, output_size - used, "Wi-Fi: %s (%d dBm)\n",
                                 (const char *)ap.ssid, ap.rssi);
    }
    if (used < output_size) {
        used += (size_t)snprintf(output + used, output_size - used, "IP: %s\n", ip);
    }
    if (used < output_size) {
        snprintf(output + used, output_size - used, "Free memory: %u KB internal, %u KB PSRAM",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / CAP_CHAT_KIB),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / CAP_CHAT_KIB));
    }
    return ESP_OK;
}

static void cap_chat_format_next_fire(char *buf, size_t size, double next_fire_ms)
{
    time_t seconds = (time_t)(next_fire_ms / CAP_CHAT_MS_PER_SECOND);
    struct tm tm_next = {0};

    if (seconds <= 0) {
        snprintf(buf, size, "-");
        return;
    }
    localtime_r(&seconds, &tm_next);
    strftime(buf, size, "%a %d/%m %H:%M", &tm_next);
}

static esp_err_t cap_chat_tasks_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output,
                                        size_t output_size)
{
    char *list_json = calloc(1, CAP_CHAT_SCHEDULER_BUF_LEN);
    cJSON *root = NULL;
    cJSON *item = NULL;
    size_t used = 0;
    esp_err_t err;

    (void)input_json;
    if (!list_json) {
        return ESP_ERR_NO_MEM;
    }
    err = claw_cap_call("scheduler_list", "{}", ctx, list_json, CAP_CHAT_SCHEDULER_BUF_LEN);
    root = (err == ESP_OK) ? cJSON_Parse(list_json) : NULL;
    free(list_json);
    if (!cJSON_IsArray(root) || cJSON_GetArraySize(root) == 0) {
        cJSON_Delete(root);
        return cap_chat_write(output, output_size, "No scheduled tasks.", ESP_OK);
    }

    used = (size_t)snprintf(output, output_size, "Scheduled tasks:\n");
    cJSON_ArrayForEach(item, root) {
        const cJSON *id = cJSON_GetObjectItem(item, "id");
        const cJSON *enabled = cJSON_GetObjectItem(item, "enabled");
        const cJSON *cron = cJSON_GetObjectItem(item, "cron_expr");
        const cJSON *next = cJSON_GetObjectItem(item, "next_fire_ms");
        char next_text[CAP_CHAT_TIME_LEN];

        if (!cJSON_IsString(id) || used >= output_size) {
            continue;
        }
        cap_chat_format_next_fire(next_text, sizeof(next_text), cJSON_IsNumber(next) ? next->valuedouble : 0);
        used += (size_t)snprintf(output + used, output_size - used, "\n%s %s\n  cron: %s\n  next: %s\n",
                                 cJSON_IsFalse(enabled) ? "[off]" : "[on]", id->valuestring,
                                 cJSON_IsString(cron) ? cron->valuestring : "-", next_text);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t cap_chat_run_execute(const char *input_json,
                                      const claw_cap_call_context_t *ctx,
                                      char *output,
                                      size_t output_size)
{
    char id[CAP_CHAT_ID_LEN] = {0};
    char call_output[128];
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    const cJSON *id_json = root ? cJSON_GetObjectItem(root, "id") : NULL;
    esp_err_t err;

    if (cJSON_IsString(id_json)) {
        snprintf(id, sizeof(id), "%s", id_json->valuestring);
    }
    cJSON_Delete(root);
    if (!id[0]) {
        return cap_chat_write(output, output_size, "Missing task id.", ESP_ERR_INVALID_ARG);
    }

    err = claw_cap_call("scheduler_trigger_now", input_json, ctx, call_output, sizeof(call_output));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "trigger %s failed: %s", id, esp_err_to_name(err));
        snprintf(output, output_size, "Could not run \"%s\" (%s). Use /tasks to see the available ones.",
                 id, esp_err_to_name(err));
        return ESP_OK;
    }
    snprintf(output, output_size, "Running \"%s\" now. The result will arrive in this chat.", id);
    return ESP_OK;
}

static const claw_cap_descriptor_t s_chat_descriptors[] = {
    {
        .id = "chat_help",
        .name = "chat_help",
        .family = "system",
        .description = "Text list of the chat slash commands.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = 0,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_chat_help_execute,
    },
    {
        .id = "chat_status",
        .name = "chat_status",
        .family = "system",
        .description = "Short text status: version, uptime, clock, Wi-Fi and memory.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_chat_status_execute,
    },
    {
        .id = "chat_web_url",
        .name = "chat_web_url",
        .family = "system",
        .description = "Text with the URL of the device settings page.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_chat_web_execute,
    },
    {
        .id = "chat_tasks",
        .name = "chat_tasks",
        .family = "system",
        .description = "Text list of scheduler entries with their next run time.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = 0,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_chat_tasks_execute,
    },
    {
        .id = "chat_run_task",
        .name = "chat_run_task",
        .family = "system",
        .description = "Trigger one scheduler entry now and answer with a short text.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = 0,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}",
        .execute = cap_chat_run_execute,
    },
};

static const claw_cap_group_t s_chat_group = {
    .group_id = "cap_system_chat",
    .descriptors = s_chat_descriptors,
    .descriptor_count = sizeof(s_chat_descriptors) / sizeof(s_chat_descriptors[0]),
};

esp_err_t cap_system_chat_register_group(void)
{
    if (claw_cap_group_exists(s_chat_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_chat_group);
}
