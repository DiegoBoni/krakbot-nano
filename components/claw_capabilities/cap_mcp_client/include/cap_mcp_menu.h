/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Registers `mcp_menu`, the chat command capability behind /mcp. */
esp_err_t cap_mcp_menu_register_group(void);

#ifdef __cplusplus
}
#endif
