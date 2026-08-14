/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAP_MCP_SERVER_ID_LEN          32
#define CAP_MCP_SERVER_NAME_LEN        64
#define CAP_MCP_SERVER_URL_LEN         256
#define CAP_MCP_SERVER_ENDPOINT_LEN    64
#define CAP_MCP_SERVER_USERNAME_LEN    64
#define CAP_MCP_SERVER_SECRET_LEN      192

/**
 * @brief  How the client authenticates to a configured remote MCP server.
 */
typedef enum {
    CAP_MCP_AUTH_NONE = 0,   /**< No credentials sent */
    CAP_MCP_AUTH_BEARER,     /**< `Authorization: Bearer <secret>` (e.g. OAuth access/API token) */
    CAP_MCP_AUTH_BASIC,      /**< HTTP Basic auth using username/secret (password) */
} cap_mcp_auth_type_t;

/**
 * @brief  A saved remote MCP server connection.
 *
 *         Persisted under the DATA storage root so it survives reboots and can be
 *         managed from the device settings web UI as well as looked up by id when
 *         the LLM invokes `mcp_list_tools` / `mcp_call_tool` with `server_id`.
 */
typedef struct {
    char id[CAP_MCP_SERVER_ID_LEN];             /**< Stable identifier, e.g. "srv_a1b2c3d4"; empty means "not yet assigned" */
    char name[CAP_MCP_SERVER_NAME_LEN];         /**< Human-friendly label, e.g. "Gmail" */
    char server_url[CAP_MCP_SERVER_URL_LEN];    /**< Remote MCP endpoint URL */
    char endpoint[CAP_MCP_SERVER_ENDPOINT_LEN]; /**< Logical endpoint name registered with esp_mcp_mgr (defaults to "mcp") */
    cap_mcp_auth_type_t auth_type;              /**< Authentication mode */
    char username[CAP_MCP_SERVER_USERNAME_LEN]; /**< Used for CAP_MCP_AUTH_BASIC */
    char secret[CAP_MCP_SERVER_SECRET_LEN];     /**< Bearer token (CAP_MCP_AUTH_BEARER) or password (CAP_MCP_AUTH_BASIC) */
    bool enabled;                               /**< Disabled servers are skipped by id-based lookups */
} cap_mcp_server_entry_t;

/**
 * @brief  List all saved MCP server entries.
 *
 * @param[out]  out_array  On success, a newly allocated cJSON array (caller owns, must cJSON_Delete).
 *                          Each element mirrors cap_mcp_server_entry_t, secret included.
 *
 * @return ESP_OK on success (an empty array if none saved), or an error code.
 */
esp_err_t cap_mcp_registry_list(cJSON **out_array);

/**
 * @brief  Look up a single saved server entry by id.
 *
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no entry has that id.
 */
esp_err_t cap_mcp_registry_get(const char *id, cap_mcp_server_entry_t *out_entry);

/**
 * @brief  Create or replace a saved server entry.
 *
 *         If `entry->id` is empty, a new id is generated and written back into
 *         `entry->id` before saving. If it is non-empty and matches an existing
 *         entry, that entry is replaced in place; otherwise a new entry with that
 *         id is appended.
 *
 * @return ESP_OK on success, or an error code.
 */
esp_err_t cap_mcp_registry_upsert(cap_mcp_server_entry_t *entry);

/**
 * @brief  Remove a saved server entry by id.
 *
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no entry has that id.
 */
esp_err_t cap_mcp_registry_delete(const char *id);

const char *cap_mcp_registry_auth_type_to_str(cap_mcp_auth_type_t type);
cap_mcp_auth_type_t cap_mcp_registry_auth_type_from_str(const char *str);

#ifdef __cplusplus
}
#endif
