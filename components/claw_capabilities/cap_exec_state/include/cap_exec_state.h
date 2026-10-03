/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Execution-state store
 *
 *         Remembers which external resources (identified by a stable key such
 *         as "email:<message-id>") an autonomous run already processed, so the
 *         agent can skip duplicates across pulses and reboots.
 *
 *         Semantics:
 *         - Entries are ordered by last mark; marking an existing key updates it,
 *           increments run_count and moves it to the most-recent position.
 *         - check() never reorders entries.
 *         - At most max_entries are kept; the least recently marked is evicted.
 *         - State is persisted as JSON to config.path. Writes are deferred by
 *           flush_delay_ms and coalesced, so a power loss can drop the marks
 *           made within that window. Writes use tmp file + rename.
 *         - A corrupt file is renamed to "<path>.bad" and the store starts empty.
 *
 *         All APIs are thread-safe but NOT ISR-safe.
 */

#define CAP_EXEC_STATE_KEY_MAX_LEN      64
#define CAP_EXEC_STATE_KIND_MAX_LEN     24
#define CAP_EXEC_STATE_SUMMARY_MAX_LEN  200
#define CAP_EXEC_STATE_FORMAT_VERSION   1

typedef enum {
    CAP_EXEC_STATE_STATUS_COMPLETED = 0,
    CAP_EXEC_STATE_STATUS_FAILED,
    CAP_EXEC_STATE_STATUS_SKIPPED,
    CAP_EXEC_STATE_STATUS_MAX,
} cap_exec_state_status_t;

typedef struct {
    char key[CAP_EXEC_STATE_KEY_MAX_LEN + 1];
    char kind[CAP_EXEC_STATE_KIND_MAX_LEN + 1];
    cap_exec_state_status_t status;
    int64_t first_seen_ms;
    int64_t last_run_ms;
    uint32_t run_count;
    char summary[CAP_EXEC_STATE_SUMMARY_MAX_LEN + 1];
} cap_exec_state_entry_t;

typedef struct {
    const char *path;       /*!< State file path; NULL uses <DATA>/state/exec_state.json */
    uint32_t max_entries;   /*!< 0 uses CONFIG_CAP_EXEC_STATE_MAX_ENTRIES */
    uint32_t max_file_bytes;/*!< 0 uses CONFIG_CAP_EXEC_STATE_MAX_FILE_BYTES */
    int32_t flush_delay_ms; /*!< <0 uses CONFIG_CAP_EXEC_STATE_FLUSH_DELAY_MS; 0 flushes on every mark */
} cap_exec_state_config_t;

#define CAP_EXEC_STATE_CONFIG_DEFAULT() { \
    .path = NULL,                         \
    .max_entries = 0,                     \
    .max_file_bytes = 0,                  \
    .flush_delay_ms = -1,                 \
}

typedef struct cap_exec_state_t *cap_exec_state_handle_t;

/** Create the store and load the persisted state (tolerates missing/corrupt files). */
esp_err_t cap_exec_state_create(const cap_exec_state_config_t *config,
                                cap_exec_state_handle_t *ret_handle);

/** Enable deferred flushing. Idempotent. */
esp_err_t cap_exec_state_start(cap_exec_state_handle_t handle);

/** Stop deferred flushing and synchronously flush pending changes. Idempotent. */
esp_err_t cap_exec_state_stop(cap_exec_state_handle_t handle);

/** Flush pending changes, release all resources and free the handle. */
esp_err_t cap_exec_state_delete(cap_exec_state_handle_t handle);

/**
 * @brief  Look up a key without altering its position
 *
 * @return ESP_OK and fills out_entry (may be NULL), ESP_ERR_NOT_FOUND, or ESP_ERR_INVALID_ARG
 */
esp_err_t cap_exec_state_check(cap_exec_state_handle_t handle,
                               const char *key,
                               cap_exec_state_entry_t *out_entry);

/**
 * @brief  Record an execution result for a key
 *
 *         kind and summary may be NULL; they are truncated UTF-8-safely.
 *
 * @return ESP_OK or ESP_ERR_INVALID_ARG (NULL/empty/too long key, bad status)
 */
esp_err_t cap_exec_state_mark(cap_exec_state_handle_t handle,
                              const char *key,
                              const char *kind,
                              cap_exec_state_status_t status,
                              const char *summary);

/**
 * @brief  Render the most recent entries (newest first) as JSON into buf
 *
 *         Output: {"count":k,"entries":[...],"truncated":true?}. limit 0 or above
 *         the capacity means all entries. If buf is too small, the oldest
 *         entries are dropped and "truncated" is set.
 *
 * @return ESP_OK, ESP_ERR_NO_MEM when not even an empty list fits, or ESP_ERR_INVALID_ARG
 */
esp_err_t cap_exec_state_list_json(cap_exec_state_handle_t handle,
                                   size_t limit,
                                   char *buf,
                                   size_t buf_size);

/** Synchronously persist pending changes (no-op when clean). */
esp_err_t cap_exec_state_flush(cap_exec_state_handle_t handle);

/** Number of entries currently stored. */
size_t cap_exec_state_count(cap_exec_state_handle_t handle);

const char *cap_exec_state_status_to_str(cap_exec_state_status_t status);
esp_err_t cap_exec_state_status_from_str(const char *str, cap_exec_state_status_t *out_status);

/** Register the cap_exec_state capability group backed by a default <DATA>/state instance. */
esp_err_t cap_exec_state_register_group(void);

#ifdef __cplusplus
}
#endif
