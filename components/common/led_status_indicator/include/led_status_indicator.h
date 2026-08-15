/*
 * SPDX-FileCopyrightText: 2026 KrakBot Nano
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
 * @brief  LED status indicator handle
 */
typedef struct led_status_indicator_t *led_status_indicator_handle_t;

/**
 * @brief  LED status indicator configuration
 */
typedef struct {
    int gpio_num;           /*!< WS2812 data GPIO */
    uint8_t led_count;      /*!< Number of LEDs chained on gpio_num */
    uint8_t max_brightness; /*!< Brightness ceiling (0-255) applied to every rendered color */
} led_status_indicator_config_t;

/**
 * @brief  Populate a config from Kconfig defaults
 */
#define LED_STATUS_INDICATOR_CONFIG_DEFAULT()                    \
    {                                                             \
        .gpio_num = CONFIG_LED_STATUS_GPIO_NUM,                   \
        .led_count = CONFIG_LED_STATUS_LED_COUNT,                 \
        .max_brightness = CONFIG_LED_STATUS_MAX_BRIGHTNESS,       \
    }

/**
 * @brief  One-shot events that briefly override the background animation
 */
typedef enum {
    LED_STATUS_EVENT_ERROR = 0, /*!< Red fast blink, ~2s */
    LED_STATUS_EVENT_MESSAGE,   /*!< White double flash */
    LED_STATUS_EVENT_CONFIG_SAVED, /*!< Green single flash */
} led_status_indicator_event_t;

/**
 * @brief  Create the LED status indicator and initialize the underlying LED strip
 *
 * @param[in]   config      Configuration; must not be NULL
 * @param[out]  ret_handle  Receives the created handle
 *
 * @return esp_err_t
 */
esp_err_t led_status_indicator_create(const led_status_indicator_config_t *config,
                                       led_status_indicator_handle_t *ret_handle);

/**
 * @brief  Start the background animation task
 *
 * Runs a boot animation once, then continuously reflects Wi-Fi connectivity
 * and agent activity until led_status_indicator_stop() is called.
 */
esp_err_t led_status_indicator_start(led_status_indicator_handle_t handle);

/**
 * @brief  Stop the background animation task and turn the LEDs off
 */
esp_err_t led_status_indicator_stop(led_status_indicator_handle_t handle);

/**
 * @brief  Delete the indicator and release the LED strip
 */
esp_err_t led_status_indicator_delete(led_status_indicator_handle_t handle);

/**
 * @brief  Queue a one-shot event animation
 *
 * Non-blocking; if the internal queue is full the event is dropped.
 */
esp_err_t led_status_indicator_notify(led_status_indicator_handle_t handle,
                                       led_status_indicator_event_t event);

#ifdef __cplusplus
}
#endif
