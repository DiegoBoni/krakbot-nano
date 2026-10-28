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

/** Registers the plain-text capabilities used by the chat slash commands. */
esp_err_t cap_system_chat_register_group(void);

#ifdef __cplusplus
}
#endif
