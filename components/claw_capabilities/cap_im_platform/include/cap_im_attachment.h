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

typedef struct {
    const char *platform;
    const char *attachment_kind;
    const char *saved_path;
    const char *saved_dir;
    const char *saved_name;
    const char *original_filename;
    const char *mime;
    const char *caption;
    const char *source_key;
    const char *source_value;
    size_t size_bytes;
    int64_t saved_at_ms;
} cap_im_attachment_payload_config_t;

const char *cap_im_attachment_ext_from_mime(const char *mime);
const char *cap_im_attachment_guess_extension(const char *path_or_url,
                                              const char *original_filename,
                                              const char *mime);
const char *cap_im_attachment_normalize_url(const char *url,
                                            char *buf,
                                            size_t buf_size);
esp_err_t cap_im_attachment_build_saved_paths(const char *root_dir,
                                              const char *platform_dir,
                                              const char *chat_id,
                                              const char *message_id,
                                              const char *kind,
                                              const char *extension,
                                              char *saved_dir,
                                              size_t saved_dir_size,
                                              char *saved_name,
                                              size_t saved_name_size,
                                              char *saved_path,
                                              size_t saved_path_size);
esp_err_t cap_im_attachment_download_url_to_file(const char *log_tag,
                                                 const char *url,
                                                 const char *dest_path,
                                                 size_t max_bytes,
                                                 size_t *out_bytes);
esp_err_t cap_im_attachment_save_buffer_to_file(const char *log_tag,
                                                const char *dest_path,
                                                const unsigned char *buf,
                                                size_t buf_len);
char *cap_im_attachment_build_payload_json(
    const cap_im_attachment_payload_config_t *config);

/**
 * @brief Delete a saved attachment once nothing downstream will read it.
 *
 * The writable DATA partition is small (a few MB); a saved attachment left
 * on disk forever fills it up. Content types with a router rule that still
 * needs the file ("audio", consumed by cap_groq_transcribe; "image" and
 * "file", consumed by cap_llm_inspect) are left alone as long as the
 * attachment_saved event actually reached the router - those capabilities
 * delete the file themselves once done with it. Everything else - including
 * when the event failed to publish, since nothing will ever consume it - is
 * deleted immediately.
 *
 * @param log_tag         Caller's ESP_LOG tag, used to report unexpected
 *                         removal failures.
 * @param content_type    The event's content_type (e.g. "audio", "image", "file").
 * @param saved_path      Path of the saved attachment on the DATA partition.
 * @param event_published Whether the attachment_saved event was published
 *                         successfully (false means nothing can process it).
 */
void cap_im_attachment_cleanup_if_unconsumed(const char *log_tag,
                                             const char *content_type,
                                             const char *saved_path,
                                             bool event_published);

#ifdef __cplusplus
}
#endif
