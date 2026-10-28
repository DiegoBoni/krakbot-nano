/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>

#include "claw_cap.h"
#include "esp_err.h"

/** `session_menu` capability: /session with a button menu (Telegram) or plain text elsewhere. */
esp_err_t cap_session_menu_execute(const char *input_json,
                                   const claw_cap_call_context_t *ctx,
                                   char *output,
                                   size_t output_size);
