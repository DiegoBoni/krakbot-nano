/*
 * SPDX-FileCopyrightText: 2026 KrakBot Nano
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "led_status_animations.h"

#include <math.h>

#define LED_STATUS_BREATHING_FLOOR 0.08f

static uint8_t scale_channel(uint8_t value, float factor)
{
    if (factor < 0.0f) {
        factor = 0.0f;
    } else if (factor > 1.0f) {
        factor = 1.0f;
    }
    return (uint8_t)((float)value * factor);
}

static led_status_rgb_t scale_color(led_status_rgb_t color, float factor)
{
    led_status_rgb_t out = {
        .r = scale_channel(color.r, factor),
        .g = scale_channel(color.g, factor),
        .b = scale_channel(color.b, factor),
    };
    return out;
}

led_status_rgb_t led_status_anim_breathing(led_status_rgb_t base, uint32_t elapsed_ms, uint32_t period_ms)
{
    if (period_ms == 0) {
        return base;
    }
    float phase = (2.0f * (float)M_PI * (float)(elapsed_ms % period_ms)) / (float)period_ms;
    float factor = (sinf(phase - (float)M_PI_2) + 1.0f) / 2.0f;
    factor = LED_STATUS_BREATHING_FLOOR + factor * (1.0f - LED_STATUS_BREATHING_FLOOR);
    return scale_color(base, factor);
}

led_status_rgb_t led_status_anim_blink(led_status_rgb_t base, uint32_t elapsed_ms, uint32_t on_ms, uint32_t off_ms)
{
    uint32_t cycle_ms = on_ms + off_ms;
    if (cycle_ms == 0) {
        return base;
    }
    uint32_t position = elapsed_ms % cycle_ms;
    if (position < on_ms) {
        return base;
    }
    led_status_rgb_t off = {0, 0, 0};
    return off;
}

led_status_rgb_t led_status_anim_pulse(led_status_rgb_t base, uint32_t elapsed_ms, uint32_t period_ms)
{
    if (period_ms == 0) {
        return base;
    }
    uint32_t half_period_ms = period_ms / 2;
    if (half_period_ms == 0) {
        return base;
    }
    uint32_t position = elapsed_ms % period_ms;
    float frac = (position < half_period_ms)
                     ? ((float)position / (float)half_period_ms)
                     : (1.0f - (float)(position - half_period_ms) / (float)half_period_ms);
    float factor = frac * frac;
    return scale_color(base, factor);
}

led_status_rgb_t led_status_anim_alternate(led_status_rgb_t color_a, led_status_rgb_t color_b,
                                            uint32_t elapsed_ms, uint32_t period_ms)
{
    if (period_ms == 0) {
        return color_a;
    }
    uint32_t half_period_ms = period_ms / 2;
    uint32_t position = elapsed_ms % period_ms;
    return (position < half_period_ms) ? color_a : color_b;
}

void led_status_anim_chase(led_status_rgb_t base, uint32_t elapsed_ms, uint32_t step_ms,
                            uint8_t led_count, led_status_rgb_t *out_pixels)
{
    if (!out_pixels || led_count == 0 || step_ms == 0) {
        return;
    }

    uint32_t head = (elapsed_ms / step_ms) % led_count;
    led_status_rgb_t trail = scale_color(base, 0.35f);
    led_status_rgb_t off = {0, 0, 0};

    for (uint8_t i = 0; i < led_count; i++) {
        if (i == head) {
            out_pixels[i] = base;
        } else if (led_count > 1 && i == (head + led_count - 1) % led_count) {
            out_pixels[i] = trail;
        } else {
            out_pixels[i] = off;
        }
    }
}

led_status_rgb_t led_status_rgb_scale(led_status_rgb_t color, uint8_t max_brightness)
{
    return scale_color(color, (float)max_brightness / 255.0f);
}
