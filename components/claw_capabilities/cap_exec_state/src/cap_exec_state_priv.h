/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "cJSON.h"
#include "cap_exec_state.h"

/** Shared JSON shape for one entry (used by the state file, list and check). */
cJSON *cap_exec_state_entry_to_cjson(const cap_exec_state_entry_t *entry);

/** Same as cap_exec_state_mark(), also returning the resulting run_count atomically. */
esp_err_t cap_exec_state_mark_ex(cap_exec_state_handle_t handle,
                                 const char *key,
                                 const char *kind,
                                 cap_exec_state_status_t status,
                                 const char *summary,
                                 uint32_t *out_run_count);
