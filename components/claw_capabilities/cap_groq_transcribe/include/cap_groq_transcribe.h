/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the cap_groq_transcribe capability group.
 *
 * Exposes a "groq_transcribe" callable capability that transcribes a local
 * audio file (e.g. a saved Telegram voice note) via the Groq Whisper API and
 * republishes the transcription as an inbound IM text message so it flows
 * back through the event router to the agent.
 */
esp_err_t cap_groq_transcribe_register_group(void);

/**
 * @brief Set the Groq API key used for transcription requests.
 */
esp_err_t cap_groq_transcribe_set_api_key(const char *api_key);

/**
 * @brief Set the Groq Whisper model id used for transcription requests.
 *
 * @param model Model id, e.g. "whisper-large-v3-turbo". If NULL or empty,
 *              the built-in default model is used.
 */
esp_err_t cap_groq_transcribe_set_model(const char *model);

/**
 * @brief Control whether the source audio file is deleted after a
 *        transcription attempt (success or failure).
 *
 * Inbound voice/audio attachments are only useful until they are
 * transcribed; leaving them on the writable DATA partition after that
 * accumulates until the partition fills up. Enabled by default.
 */
esp_err_t cap_groq_transcribe_set_delete_audio_after_transcribe(bool enable);

#ifdef __cplusplus
}
#endif
