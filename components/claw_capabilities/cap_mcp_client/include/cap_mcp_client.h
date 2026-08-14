/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the MCP client capability group with the claw_cap registry.
 *
 * Registers group id `cap_mcp_client` with callable descriptors `mcp_list_tools`,
 * `mcp_call_tool`, `mcp_list_servers`, and `mcp_discover` (remote tools/list, tools/call,
 * saved-server listing, and local-network discovery). Safe to call repeatedly: if the
 * group already exists, returns `ESP_OK` without registering again.
 * This only registers capability metadata and callbacks; remote MCP requests are
 * made later when one of those capabilities is invoked.
 *
 * @return ESP_OK on success, or an error code from `claw_cap_register_group()`.
 */
esp_err_t cap_mcp_client_register_group(void);

/**
 * @brief Test connectivity/auth against a saved MCP server by issuing a `tools/list` call.
 *
 * Intended for the settings web UI's "Test connection" action on a saved server entry.
 *
 * @param[in]   server_id     Id of a server saved via the MCP registry (cap_mcp_registry.h).
 * @param[out]  message       Buffer to receive a short human-readable result, always
 *                             NUL-terminated on return (e.g. "OK: 4 tool(s) found" or
 *                             "Error: ...").
 * @param[in]   message_size  Size of message in bytes.
 *
 * @return ESP_OK if the request/response cycle completed (even if the server reported an
 *         MCP-level error, which is reflected in `message`); an error code if the server
 *         id is unknown, disabled, or the transport failed outright.
 */
esp_err_t cap_mcp_client_test_server(const char *server_id, char *message, size_t message_size);

#ifdef __cplusplus
}
#endif
