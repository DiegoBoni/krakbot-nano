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

#define CAP_ACTIVITY_LOG_ACTION_MAX   96
#define CAP_ACTIVITY_LOG_REASON_MAX   128
#define CAP_ACTIVITY_LOG_REF_MAX      128
#define CAP_ACTIVITY_LOG_DETAIL_MAX   200
#define CAP_ACTIVITY_LOG_LIST_MAX     50
#define CAP_ACTIVITY_LOG_LIST_DEFAULT 20

typedef enum {
    CAP_ACTIVITY_ORIGIN_PROACTIVE_PULSE = 0,
    CAP_ACTIVITY_ORIGIN_SCHEDULED_TASK,
    CAP_ACTIVITY_ORIGIN_MANUAL,
    CAP_ACTIVITY_ORIGIN_MAX,  /**< Not a valid origin; used as "no filter" in list_json */
} cap_activity_origin_t;

typedef enum {
    CAP_ACTIVITY_RESULT_COMPLETED = 0,
    CAP_ACTIVITY_RESULT_SKIPPED,
    CAP_ACTIVITY_RESULT_ERROR,
    CAP_ACTIVITY_RESULT_MAX,
} cap_activity_result_t;

typedef struct {
    const char *base_dir;   /**< Log directory; NULL = "<DATA>/activity" */
    uint32_t max_bytes;     /**< Max bytes per segment; 0 = CONFIG_CAP_ACTIVITY_LOG_MAX_BYTES */
} cap_activity_log_config_t;

typedef struct {
    uint32_t pending;       /**< Entries buffered in RAM, not yet on flash */
    uint32_t dropped;       /**< Entries lost (buffer full or append timeout) */
    uint32_t flush_errors;  /**< Failed flash writes */
    uint32_t rotations;     /**< Segment rotations since start */
} cap_activity_log_stats_t;

typedef struct cap_activity_log_t *cap_activity_log_handle_t;

/**
 * @brief  Create the activity log (singleton)
 *
 * @return
 *         - ESP_OK on success
 *         - ESP_ERR_INVALID_ARG if ret_handle is NULL
 *         - ESP_ERR_INVALID_STATE if an instance already exists
 *         - ESP_ERR_NO_MEM / ESP_ERR_INVALID_SIZE on allocation or path errors
 */
esp_err_t cap_activity_log_create(const cap_activity_log_config_t *config,
                                  cap_activity_log_handle_t *ret_handle);

/** @brief  Create the directory, recover state from disk and start the flush task. */
esp_err_t cap_activity_log_start(cap_activity_log_handle_t handle);

/** @brief  Stop the flush task and flush all pending entries to flash. */
esp_err_t cap_activity_log_stop(cap_activity_log_handle_t handle);

/** @brief  Stop (if running) and free all resources. */
/**
 * @note Concurrency contract: callers must make sure no cap_activity_log_append()/list call is in flight
 *       when stop/delete runs. In the firmware the log lives for the whole uptime and is never stopped,
 *       so this only matters for tests and for the create-then-start-failed cleanup path.
 */
esp_err_t cap_activity_log_delete(cap_activity_log_handle_t handle);

/**
 * @brief  Append an entry to the in-RAM buffer
 *
 *         Non-blocking, no I/O and no allocation: waits at most 5 ms for the
 *         buffer lock. Strings are truncated on UTF-8 boundaries. @p ref and
 *         @p detail may be NULL or empty.
 *
 * @return
 *         - ESP_OK on success
 *         - ESP_ERR_INVALID_STATE if the log is not started
 *         - ESP_ERR_INVALID_ARG on bad enum, or NULL/empty action or reason
 *         - ESP_ERR_TIMEOUT if the buffer lock was busy (entry dropped)
 */
esp_err_t cap_activity_log_append(cap_activity_origin_t origin,
                                  const char *action,
                                  const char *reason,
                                  cap_activity_result_t result,
                                  const char *ref,
                                  const char *detail);

/**
 * @brief  Write the most recent matching entries as a JSON array (newest first)
 *
 *         Includes entries still pending in RAM. Corrupt lines on flash are skipped.
 *
 * @param[in]   limit          0 = LIST_DEFAULT; values above LIST_MAX are clamped
 * @param[in]   origin_filter  CAP_ACTIVITY_ORIGIN_MAX = any origin
 * @param[in]   since_ms       Only entries with ts >= since_ms
 * @param[out]  out            Output buffer (always NUL-terminated, valid JSON array)
 * @param[in]   out_size       Size of out
 * @param[out]  out_count      Number of entries written (may be NULL)
 *
 * @return
 *         - ESP_OK on success
 *         - ESP_ERR_INVALID_STATE if the log is not started
 *         - ESP_ERR_INVALID_ARG on bad arguments
 *         - ESP_ERR_INVALID_SIZE if out was too small (array closed with the entries that fit)
 *         - ESP_ERR_NO_MEM on temporary allocation failure
 */
esp_err_t cap_activity_log_list_json(size_t limit,
                                     cap_activity_origin_t origin_filter,
                                     int64_t since_ms,
                                     char *out,
                                     size_t out_size,
                                     size_t *out_count);

/**
 * @brief  Same as cap_activity_log_list_json() with an exact-match filter on the entry ref
 *
 *         The filter is applied before @p limit, so limit=1 returns the newest entry of that ref even
 *         when newer entries of other refs exist. NULL or empty @p ref_filter = any ref.
 */
esp_err_t cap_activity_log_list_json_ref(size_t limit,
                                         cap_activity_origin_t origin_filter,
                                         const char *ref_filter,
                                         int64_t since_ms,
                                         char *out,
                                         size_t out_size,
                                         size_t *out_count);

esp_err_t cap_activity_log_get_stats(cap_activity_log_stats_t *out);

/** @brief  Enum <-> string conversions; NULL or unknown strings give ESP_ERR_INVALID_ARG. */
const char *cap_activity_origin_to_str(cap_activity_origin_t origin);
const char *cap_activity_result_to_str(cap_activity_result_t result);
esp_err_t cap_activity_origin_from_str(const char *str, cap_activity_origin_t *out);
esp_err_t cap_activity_result_from_str(const char *str, cap_activity_result_t *out);

/** @brief  Register the activity_log_list / activity_log_append capability group. */
esp_err_t cap_activity_log_register_group(void);

#ifdef __cplusplus
}
#endif
