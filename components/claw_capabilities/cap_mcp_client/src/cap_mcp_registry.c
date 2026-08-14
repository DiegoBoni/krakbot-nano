/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_mcp_registry.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_random.h"

#include "claw_paths.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define CAP_MCP_REGISTRY_RELATIVE_PATH "mcp/servers.json"
#define CAP_MCP_REGISTRY_RELATIVE_DIR  "mcp"

static const char *TAG = "cap_mcp_registry";
static SemaphoreHandle_t s_lock;

static SemaphoreHandle_t cap_mcp_registry_lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock;
}

static esp_err_t cap_mcp_registry_file_path(char *out, size_t out_size)
{
    return claw_paths_join(CLAW_PATH_DATA, CAP_MCP_REGISTRY_RELATIVE_PATH, out, out_size);
}

static esp_err_t cap_mcp_registry_ensure_dir(void)
{
    char dir_path[CAP_MCP_SERVER_URL_LEN];
    esp_err_t err = claw_paths_join(CLAW_PATH_DATA, CAP_MCP_REGISTRY_RELATIVE_DIR, dir_path, sizeof(dir_path));
    if (err != ESP_OK) {
        return err;
    }
    if (mkdir(dir_path, 0775) != 0) {
        struct stat st = {0};
        if (stat(dir_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            ESP_LOGE(TAG, "Failed to create %s", dir_path);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

/* Caller must hold s_lock. Returns a root object with a "servers" array (never NULL on ESP_OK). */
static esp_err_t cap_mcp_registry_load_locked(cJSON **out_root)
{
    char path[CAP_MCP_SERVER_URL_LEN];
    esp_err_t err = cap_mcp_registry_file_path(path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = NULL;
    FILE *file = fopen(path, "rb");
    if (file) {
        fseek(file, 0, SEEK_END);
        long size = ftell(file);
        fseek(file, 0, SEEK_SET);
        if (size > 0 && size < (1024 * 64)) {
            char *buf = malloc((size_t)size + 1);
            if (buf) {
                size_t read_bytes = fread(buf, 1, (size_t)size, file);
                buf[read_bytes] = '\0';
                root = cJSON_Parse(buf);
                free(buf);
            }
        }
        fclose(file);
    }

    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
        if (!root) {
            return ESP_ERR_NO_MEM;
        }
    }

    cJSON *servers = cJSON_GetObjectItem(root, "servers");
    if (!cJSON_IsArray(servers)) {
        if (servers) {
            cJSON_DeleteItemFromObject(root, "servers");
        }
        servers = cJSON_AddArrayToObject(root, "servers");
        if (!servers) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
    }

    *out_root = root;
    return ESP_OK;
}

/* Caller must hold s_lock. */
static esp_err_t cap_mcp_registry_save_locked(cJSON *root)
{
    char path[CAP_MCP_SERVER_URL_LEN];
    esp_err_t err = cap_mcp_registry_file_path(path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }

    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }

    FILE *file = fopen(path, "wb");
    if (!file) {
        if (cap_mcp_registry_ensure_dir() != ESP_OK) {
            cJSON_free(json_str);
            return ESP_FAIL;
        }
        file = fopen(path, "wb");
    }
    if (!file) {
        cJSON_free(json_str);
        return ESP_FAIL;
    }

    size_t len = strlen(json_str);
    size_t written = fwrite(json_str, 1, len, file);
    fclose(file);
    cJSON_free(json_str);
    return (written == len) ? ESP_OK : ESP_FAIL;
}

static void cap_mcp_registry_entry_to_json(const cap_mcp_server_entry_t *entry, cJSON *item)
{
    cJSON_AddStringToObject(item, "id", entry->id);
    cJSON_AddStringToObject(item, "name", entry->name);
    cJSON_AddStringToObject(item, "server_url", entry->server_url);
    cJSON_AddStringToObject(item, "endpoint", entry->endpoint);
    cJSON_AddStringToObject(item, "auth_type", cap_mcp_registry_auth_type_to_str(entry->auth_type));
    cJSON_AddStringToObject(item, "username", entry->username);
    cJSON_AddStringToObject(item, "secret", entry->secret);
    cJSON_AddBoolToObject(item, "enabled", entry->enabled);
}

static void cap_mcp_registry_entry_from_json(const cJSON *item, cap_mcp_server_entry_t *entry)
{
    memset(entry, 0, sizeof(*entry));

    const cJSON *id = cJSON_GetObjectItem(item, "id");
    const cJSON *name = cJSON_GetObjectItem(item, "name");
    const cJSON *server_url = cJSON_GetObjectItem(item, "server_url");
    const cJSON *endpoint = cJSON_GetObjectItem(item, "endpoint");
    const cJSON *auth_type = cJSON_GetObjectItem(item, "auth_type");
    const cJSON *username = cJSON_GetObjectItem(item, "username");
    const cJSON *secret = cJSON_GetObjectItem(item, "secret");
    const cJSON *enabled = cJSON_GetObjectItem(item, "enabled");

    if (cJSON_IsString(id)) strlcpy(entry->id, id->valuestring, sizeof(entry->id));
    if (cJSON_IsString(name)) strlcpy(entry->name, name->valuestring, sizeof(entry->name));
    if (cJSON_IsString(server_url)) strlcpy(entry->server_url, server_url->valuestring, sizeof(entry->server_url));
    if (cJSON_IsString(endpoint)) strlcpy(entry->endpoint, endpoint->valuestring, sizeof(entry->endpoint));
    entry->auth_type = cJSON_IsString(auth_type) ? cap_mcp_registry_auth_type_from_str(auth_type->valuestring) : CAP_MCP_AUTH_NONE;
    if (cJSON_IsString(username)) strlcpy(entry->username, username->valuestring, sizeof(entry->username));
    if (cJSON_IsString(secret)) strlcpy(entry->secret, secret->valuestring, sizeof(entry->secret));
    entry->enabled = cJSON_IsBool(enabled) ? cJSON_IsTrue(enabled) : true;
}

const char *cap_mcp_registry_auth_type_to_str(cap_mcp_auth_type_t type)
{
    switch (type) {
    case CAP_MCP_AUTH_BEARER: return "bearer";
    case CAP_MCP_AUTH_BASIC:  return "basic";
    default:                  return "none";
    }
}

cap_mcp_auth_type_t cap_mcp_registry_auth_type_from_str(const char *str)
{
    if (str && strcmp(str, "bearer") == 0) return CAP_MCP_AUTH_BEARER;
    if (str && strcmp(str, "basic") == 0) return CAP_MCP_AUTH_BASIC;
    return CAP_MCP_AUTH_NONE;
}

esp_err_t cap_mcp_registry_list(cJSON **out_array)
{
    if (!out_array) {
        return ESP_ERR_INVALID_ARG;
    }

    SemaphoreHandle_t lock = cap_mcp_registry_lock();
    xSemaphoreTake(lock, portMAX_DELAY);

    cJSON *root = NULL;
    esp_err_t err = cap_mcp_registry_load_locked(&root);
    if (err != ESP_OK) {
        xSemaphoreGive(lock);
        return err;
    }

    cJSON *servers = cJSON_DetachItemFromObject(root, "servers");
    cJSON_Delete(root);
    xSemaphoreGive(lock);

    *out_array = servers ? servers : cJSON_CreateArray();
    return *out_array ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t cap_mcp_registry_get(const char *id, cap_mcp_server_entry_t *out_entry)
{
    if (!id || !id[0] || !out_entry) {
        return ESP_ERR_INVALID_ARG;
    }

    SemaphoreHandle_t lock = cap_mcp_registry_lock();
    xSemaphoreTake(lock, portMAX_DELAY);

    cJSON *root = NULL;
    esp_err_t err = cap_mcp_registry_load_locked(&root);
    if (err != ESP_OK) {
        xSemaphoreGive(lock);
        return err;
    }

    cJSON *servers = cJSON_GetObjectItem(root, "servers");
    esp_err_t result = ESP_ERR_NOT_FOUND;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, servers) {
        cJSON *item_id = cJSON_GetObjectItem(item, "id");
        if (cJSON_IsString(item_id) && strcmp(item_id->valuestring, id) == 0) {
            cap_mcp_registry_entry_from_json(item, out_entry);
            result = ESP_OK;
            break;
        }
    }

    cJSON_Delete(root);
    xSemaphoreGive(lock);
    return result;
}

esp_err_t cap_mcp_registry_upsert(cap_mcp_server_entry_t *entry)
{
    if (!entry) {
        return ESP_ERR_INVALID_ARG;
    }

    SemaphoreHandle_t lock = cap_mcp_registry_lock();
    xSemaphoreTake(lock, portMAX_DELAY);

    cJSON *root = NULL;
    esp_err_t err = cap_mcp_registry_load_locked(&root);
    if (err != ESP_OK) {
        xSemaphoreGive(lock);
        return err;
    }

    cJSON *servers = cJSON_GetObjectItem(root, "servers");

    if (!entry->id[0]) {
        char candidate[CAP_MCP_SERVER_ID_LEN];
        bool unique = false;
        while (!unique) {
            snprintf(candidate, sizeof(candidate), "srv_%08" PRIx32, esp_random());
            unique = true;
            cJSON *existing = NULL;
            cJSON_ArrayForEach(existing, servers) {
                cJSON *existing_id = cJSON_GetObjectItem(existing, "id");
                if (cJSON_IsString(existing_id) && strcmp(existing_id->valuestring, candidate) == 0) {
                    unique = false;
                    break;
                }
            }
        }
        strlcpy(entry->id, candidate, sizeof(entry->id));
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, servers) {
        cJSON *item_id = cJSON_GetObjectItem(item, "id");
        if (cJSON_IsString(item_id) && strcmp(item_id->valuestring, entry->id) == 0) {
            break;
        }
    }

    cJSON *new_item = cJSON_CreateObject();
    if (!new_item) {
        cJSON_Delete(root);
        xSemaphoreGive(lock);
        return ESP_ERR_NO_MEM;
    }
    cap_mcp_registry_entry_to_json(entry, new_item);

    if (item) {
        cJSON_ReplaceItemViaPointer(servers, item, new_item);
    } else {
        cJSON_AddItemToArray(servers, new_item);
    }

    err = cap_mcp_registry_save_locked(root);
    cJSON_Delete(root);
    xSemaphoreGive(lock);
    return err;
}

esp_err_t cap_mcp_registry_delete(const char *id)
{
    if (!id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    SemaphoreHandle_t lock = cap_mcp_registry_lock();
    xSemaphoreTake(lock, portMAX_DELAY);

    cJSON *root = NULL;
    esp_err_t err = cap_mcp_registry_load_locked(&root);
    if (err != ESP_OK) {
        xSemaphoreGive(lock);
        return err;
    }

    cJSON *servers = cJSON_GetObjectItem(root, "servers");
    int index = 0;
    esp_err_t result = ESP_ERR_NOT_FOUND;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, servers) {
        cJSON *item_id = cJSON_GetObjectItem(item, "id");
        if (cJSON_IsString(item_id) && strcmp(item_id->valuestring, id) == 0) {
            cJSON_DeleteItemFromArray(servers, index);
            result = ESP_OK;
            break;
        }
        index++;
    }

    if (result == ESP_OK) {
        err = cap_mcp_registry_save_locked(root);
        if (err != ESP_OK) {
            result = err;
        }
    }

    cJSON_Delete(root);
    xSemaphoreGive(lock);
    return result;
}
