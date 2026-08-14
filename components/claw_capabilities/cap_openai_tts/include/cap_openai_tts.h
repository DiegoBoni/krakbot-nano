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

/**
 * @brief Register the cap_openai_tts capability group.
 *
 * Exposes a "openai_tts_speak" callable capability that synthesizes speech
 * for a piece of text via the OpenAI text-to-speech API and sends the
 * resulting audio back to the originating IM chat as a voice note.
 */
esp_err_t cap_openai_tts_register_group(void);

/** Set the OpenAI API key used for TTS requests. */
esp_err_t cap_openai_tts_set_api_key(const char *api_key);

/** Set the OpenAI TTS voice (e.g. "alloy"). Empty/NULL resets to default. */
esp_err_t cap_openai_tts_set_voice(const char *voice);

/** Set the OpenAI TTS model (e.g. "tts-1"). Empty/NULL resets to default. */
esp_err_t cap_openai_tts_set_model(const char *model);

/** Set the TTS provider: "openai" (default) or "groq". */
esp_err_t cap_openai_tts_set_provider(const char *provider);

/** Set the writable directory used to store generated audio files. */
esp_err_t cap_openai_tts_set_cache_dir(const char *cache_dir);

#ifdef __cplusplus
}
#endif
