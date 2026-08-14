/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <string.h>

#include "cap_mcp_client.h"
#include "cap_mcp_registry.h"

static void mcp_entry_from_json_body(cJSON *body, cap_mcp_server_entry_t *entry)
{
    memset(entry, 0, sizeof(*entry));

    http_server_json_read_string(body, "id", entry->id, sizeof(entry->id));
    http_server_json_read_string(body, "name", entry->name, sizeof(entry->name));
    http_server_json_read_string(body, "server_url", entry->server_url, sizeof(entry->server_url));
    http_server_json_read_string(body, "endpoint", entry->endpoint, sizeof(entry->endpoint));
    http_server_json_read_string(body, "username", entry->username, sizeof(entry->username));
    http_server_json_read_string(body, "secret", entry->secret, sizeof(entry->secret));

    char auth_type_str[16] = {0};
    http_server_json_read_string(body, "auth_type", auth_type_str, sizeof(auth_type_str));
    entry->auth_type = cap_mcp_registry_auth_type_from_str(auth_type_str);

    cJSON *enabled_item = cJSON_GetObjectItemCaseSensitive(body, "enabled");
    entry->enabled = cJSON_IsBool(enabled_item) ? cJSON_IsTrue(enabled_item) : true;
}

static cJSON *mcp_entry_to_json(const cap_mcp_server_entry_t *entry)
{
    cJSON *item = cJSON_CreateObject();
    if (!item) {
        return NULL;
    }
    http_server_json_add_string(item, "id", entry->id);
    http_server_json_add_string(item, "name", entry->name);
    http_server_json_add_string(item, "server_url", entry->server_url);
    http_server_json_add_string(item, "endpoint", entry->endpoint);
    http_server_json_add_string(item, "auth_type", cap_mcp_registry_auth_type_to_str(entry->auth_type));
    http_server_json_add_string(item, "username", entry->username);
    http_server_json_add_string(item, "secret", entry->secret);
    cJSON_AddBoolToObject(item, "enabled", entry->enabled);
    return item;
}

static esp_err_t mcp_servers_get_handler(httpd_req_t *req)
{
    if (http_server_require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *servers = NULL;
    esp_err_t err = cap_mcp_registry_list(&servers);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read MCP server registry");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(servers);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(root, "servers", servers);
    return http_server_send_json_response(req, root);
}

static esp_err_t mcp_servers_post_handler(httpd_req_t *req)
{
    if (http_server_require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *body = NULL;
    if (http_server_parse_json_body(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }

    cap_mcp_server_entry_t entry = {0};
    mcp_entry_from_json_body(body, &entry);
    cJSON_Delete(body);

    if (!entry.name[0] || !entry.server_url[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name and server_url are required");
    }
    if (!entry.endpoint[0]) {
        strlcpy(entry.endpoint, "mcp", sizeof(entry.endpoint));
    }

    esp_err_t err = cap_mcp_registry_upsert(&entry);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save MCP server");
    }

    cJSON *item = mcp_entry_to_json(&entry);
    if (!item) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    return http_server_send_json_response(req, item);
}

static esp_err_t mcp_servers_delete_handler(httpd_req_t *req)
{
    if (http_server_require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    char id[CAP_MCP_SERVER_ID_LEN] = {0};
    if (http_server_query_get(req, "id", id, sizeof(id)) != ESP_OK || !id[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
    }

    esp_err_t err = cap_mcp_registry_delete(id);
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Server not found");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete MCP server");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t mcp_servers_test_handler(httpd_req_t *req)
{
    if (http_server_require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *body = NULL;
    if (http_server_parse_json_body(req, &body) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }

    char id[CAP_MCP_SERVER_ID_LEN] = {0};
    http_server_json_read_string(body, "id", id, sizeof(id));
    cJSON_Delete(body);

    if (!id[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
    }

    char message[192] = {0};
    esp_err_t err = cap_mcp_client_test_server(id, message, sizeof(message));

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    http_server_json_add_string(root, "message", message);
    return http_server_send_json_response(req, root);
}

esp_err_t http_server_register_mcp_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/mcp/servers", .method = HTTP_GET, .handler = mcp_servers_get_handler },
        { .uri = "/api/mcp/servers", .method = HTTP_POST, .handler = mcp_servers_post_handler },
        { .uri = "/api/mcp/servers", .method = HTTP_DELETE, .handler = mcp_servers_delete_handler },
        { .uri = "/api/mcp/servers/test", .method = HTTP_POST, .handler = mcp_servers_test_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
