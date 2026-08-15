/*
 * SPDX-FileCopyrightText: 2026 KrakBot Nano
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} led_status_rgb_t;

/* Smooth sine breathing envelope over base color, one full cycle per period_ms. */
led_status_rgb_t led_status_anim_breathing(led_status_rgb_t base, uint32_t elapsed_ms, uint32_t period_ms);

/* Hard on/off blink: base color for on_ms, then off for off_ms, repeating. */
led_status_rgb_t led_status_anim_blink(led_status_rgb_t base, uint32_t elapsed_ms, uint32_t on_ms, uint32_t off_ms);

/* Sharper ease-in/ease-out pulse than breathing, one full cycle per period_ms. */
led_status_rgb_t led_status_anim_pulse(led_status_rgb_t base, uint32_t elapsed_ms, uint32_t period_ms);

/* Alternates between color_a and color_b every period_ms/2. */
led_status_rgb_t led_status_anim_alternate(led_status_rgb_t color_a, led_status_rgb_t color_b,
                                            uint32_t elapsed_ms, uint32_t period_ms);

/* Comet-style chase: writes led_count pixels into out_pixels, moving one step per step_ms. */
void led_status_anim_chase(led_status_rgb_t base, uint32_t elapsed_ms, uint32_t step_ms,
                            uint8_t led_count, led_status_rgb_t *out_pixels);

/* Scales each channel by max_brightness/255. */
led_status_rgb_t led_status_rgb_scale(led_status_rgb_t color, uint8_t max_brightness);

#ifdef __cplusplus
}
#endif
