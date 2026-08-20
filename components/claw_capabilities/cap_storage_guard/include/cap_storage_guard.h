/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the cap_storage_guard capability group.
 *
 * Exposes "storage_status" and "storage_cleanup", LLM-callable capabilities
 * that report DATA partition usage and delete reclaimable leftover files
 * (saved IM attachments, orphaned TTS cache audio) on request.
 */
esp_err_t cap_storage_guard_register_group(void);

typedef struct {
    /** Alert once used bytes on the DATA partition reach this value. */
    uint32_t threshold_bytes;
    /** How often to poll DATA partition usage. */
    uint32_t check_interval_ms;
    /** Directory of saved IM attachments (photos/documents/leftover audio); reclaimable. */
    const char *attachment_root_dir;
    /** Directory of TTS synthesis cache files; reclaimable. */
    const char *tts_cache_root;
} cap_storage_guard_config_t;

/**
 * @brief Start the periodic storage watchdog task.
 *
 * Polls DATA partition usage every config->check_interval_ms; the first time
 * usage reaches config->threshold_bytes, sends one alert to the configured
 * admin (see cap_storage_guard_set_admin) describing usage and inviting a
 * cleanup request. The alert re-arms only after usage drops meaningfully
 * back below the threshold, so a single overflow does not spam the admin.
 *
 * A zero threshold_bytes disables the watchdog (no task is created).
 */
esp_err_t cap_storage_guard_start(const cap_storage_guard_config_t *config);

/**
 * @brief Stop the storage watchdog task, if running.
 */
esp_err_t cap_storage_guard_stop(void);

/**
 * @brief Set the destination for storage alarm notifications.
 *
 * @param channel  Source channel of the admin chat, e.g. "telegram" (matches
 *                 the channel values used elsewhere for send_message caps).
 * @param chat_id  Chat id to notify, on that channel.
 */
esp_err_t cap_storage_guard_set_admin(const char *channel, const char *chat_id);

#ifdef __cplusplus
}
#endif
