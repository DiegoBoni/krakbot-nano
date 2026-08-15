/*
 * SPDX-FileCopyrightText: 2026 KrakBot Nano
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "led_status_indicator.h"

#include <stdlib.h>

#include "app_claw.h"
#include "claw_core.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led_status_animations.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "led_strip_types.h"
#include "wifi_manager.h"

static const char *TAG = "led_status";

/* Stack buffers are sized for this cap; Kconfig allows up to 64 LEDs but
 * this indicator targets small onboard strips (see LED_STATUS_LED_COUNT). */
#define LED_STATUS_MAX_LEDS 16

#define LED_STATUS_TASK_PERIOD_MS               33
#define LED_STATUS_TASK_STACK_SIZE              3072
#define LED_STATUS_TASK_PRIORITY                3
#define LED_STATUS_EVENT_QUEUE_LEN              4
#define LED_STATUS_RMT_RESOLUTION_HZ             (10 * 1000 * 1000)

#define LED_STATUS_IDLE_BREATH_PERIOD_MS        4000
#define LED_STATUS_THINKING_STEP_MS             180
#define LED_STATUS_TOOL_PULSE_PERIOD_MS         500
#define LED_STATUS_WIFI_CONNECTING_PERIOD_MS    700
#define LED_STATUS_WIFI_AP_PERIOD_MS            1200
#define LED_STATUS_WIFI_DISCONNECTED_ON_MS      150
#define LED_STATUS_WIFI_DISCONNECTED_OFF_MS     1850
#define LED_STATUS_WIFI_CONNECTED_FLASH_MS      600
#define LED_STATUS_ERROR_DURATION_MS            2000
#define LED_STATUS_ERROR_BLINK_ON_MS            120
#define LED_STATUS_ERROR_BLINK_OFF_MS           120
#define LED_STATUS_MESSAGE_FLASH_ON_MS          150
#define LED_STATUS_MESSAGE_FLASH_OFF_MS         150
#define LED_STATUS_MESSAGE_FLASH_COUNT          2
#define LED_STATUS_CONFIG_SAVED_FLASH_MS        400
#define LED_STATUS_BOOT_SWEEP_STEP_MS           120

static const led_status_rgb_t COLOR_OFF                  = {0, 0, 0};
static const led_status_rgb_t COLOR_BOOT_WHITE            = {255, 255, 255};
static const led_status_rgb_t COLOR_IDLE_GREEN            = {0, 200, 60};
static const led_status_rgb_t COLOR_THINKING_ORANGE       = {255, 110, 0};
static const led_status_rgb_t COLOR_TOOL_AMBER            = {255, 170, 0};
static const led_status_rgb_t COLOR_WIFI_CONNECTING_BLUE  = {0, 90, 255};
static const led_status_rgb_t COLOR_WIFI_CONNECTED_GREEN  = {0, 255, 90};
static const led_status_rgb_t COLOR_WIFI_AP_VIOLET        = {140, 0, 220};
static const led_status_rgb_t COLOR_WIFI_DISCONNECTED_RED = {200, 0, 0};
static const led_status_rgb_t COLOR_ERROR_RED              = {255, 0, 0};
static const led_status_rgb_t COLOR_MESSAGE_WHITE          = {255, 255, 255};
static const led_status_rgb_t COLOR_CONFIG_SAVED_GREEN     = {0, 255, 60};

typedef enum {
    LED_STATUS_MODE_IDLE = 0,
    LED_STATUS_MODE_THINKING,
    LED_STATUS_MODE_RUNNING_TOOL,
    LED_STATUS_MODE_WIFI_CONNECTING,
    LED_STATUS_MODE_WIFI_AP,
    LED_STATUS_MODE_WIFI_DISCONNECTED,
} led_status_bg_mode_t;

/* Superset of led_status_indicator_event_t: adds internally-triggered events
 * that are not part of the public API. */
typedef enum {
    LED_STATUS_QEVT_ERROR = LED_STATUS_EVENT_ERROR,
    LED_STATUS_QEVT_MESSAGE = LED_STATUS_EVENT_MESSAGE,
    LED_STATUS_QEVT_CONFIG_SAVED = LED_STATUS_EVENT_CONFIG_SAVED,
    LED_STATUS_QEVT_WIFI_CONNECTED_FLASH,
} led_status_queue_event_t;

struct led_status_indicator_t {
    led_strip_handle_t strip;
    int gpio_num;
    uint8_t led_count;
    uint8_t max_brightness;

    TaskHandle_t task;
    QueueHandle_t event_queue;
    volatile bool running;

    bool wifi_ever_connected;
};

static esp_err_t render_pixels(led_status_indicator_handle_t handle, const led_status_rgb_t *pixels)
{
    for (uint8_t i = 0; i < handle->led_count; i++) {
        led_status_rgb_t scaled = led_status_rgb_scale(pixels[i], handle->max_brightness);
        led_strip_set_pixel(handle->strip, i, scaled.r, scaled.g, scaled.b);
    }
    return led_strip_refresh(handle->strip);
}

static void render_solid(led_status_indicator_handle_t handle, led_status_rgb_t color)
{
    led_status_rgb_t pixels[LED_STATUS_MAX_LEDS];
    for (uint8_t i = 0; i < handle->led_count; i++) {
        pixels[i] = color;
    }
    render_pixels(handle, pixels);
}

static led_status_bg_mode_t arbitrate_mode(led_status_indicator_handle_t handle,
                                            const wifi_manager_status_t *wifi_status,
                                            claw_core_agent_loop_phase_t phase)
{
    if (wifi_status->ap_active) {
        return LED_STATUS_MODE_WIFI_AP;
    }
    if (wifi_status->sta_configured && !wifi_status->sta_connected) {
        return handle->wifi_ever_connected ? LED_STATUS_MODE_WIFI_DISCONNECTED : LED_STATUS_MODE_WIFI_CONNECTING;
    }
    if (phase == CLAW_CORE_AGENT_LOOP_PHASE_RUNNING_TOOL) {
        return LED_STATUS_MODE_RUNNING_TOOL;
    }
    if (phase != CLAW_CORE_AGENT_LOOP_PHASE_IDLE) {
        return LED_STATUS_MODE_THINKING;
    }
    return LED_STATUS_MODE_IDLE;
}

static void render_mode(led_status_indicator_handle_t handle, led_status_bg_mode_t mode, uint32_t elapsed_ms)
{
    if (mode == LED_STATUS_MODE_THINKING) {
        led_status_rgb_t pixels[LED_STATUS_MAX_LEDS];
        led_status_anim_chase(COLOR_THINKING_ORANGE, elapsed_ms, LED_STATUS_THINKING_STEP_MS, handle->led_count, pixels);
        render_pixels(handle, pixels);
        return;
    }

    led_status_rgb_t color;
    switch (mode) {
    case LED_STATUS_MODE_RUNNING_TOOL:
        color = led_status_anim_pulse(COLOR_TOOL_AMBER, elapsed_ms, LED_STATUS_TOOL_PULSE_PERIOD_MS);
        break;
    case LED_STATUS_MODE_WIFI_CONNECTING:
        color = led_status_anim_pulse(COLOR_WIFI_CONNECTING_BLUE, elapsed_ms, LED_STATUS_WIFI_CONNECTING_PERIOD_MS);
        break;
    case LED_STATUS_MODE_WIFI_AP:
        color = led_status_anim_alternate(COLOR_WIFI_CONNECTING_BLUE, COLOR_WIFI_AP_VIOLET, elapsed_ms,
                                          LED_STATUS_WIFI_AP_PERIOD_MS);
        break;
    case LED_STATUS_MODE_WIFI_DISCONNECTED:
        color = led_status_anim_blink(COLOR_WIFI_DISCONNECTED_RED, elapsed_ms, LED_STATUS_WIFI_DISCONNECTED_ON_MS,
                                      LED_STATUS_WIFI_DISCONNECTED_OFF_MS);
        break;
    case LED_STATUS_MODE_IDLE:
    default:
        color = led_status_anim_breathing(COLOR_IDLE_GREEN, elapsed_ms, LED_STATUS_IDLE_BREATH_PERIOD_MS);
        break;
    }
    render_solid(handle, color);
}

static void run_boot_animation(led_status_indicator_handle_t handle)
{
    led_status_rgb_t pixels[LED_STATUS_MAX_LEDS];
    uint32_t total_ms = (uint32_t)handle->led_count * LED_STATUS_BOOT_SWEEP_STEP_MS;
    for (uint32_t t = 0; t <= total_ms; t += LED_STATUS_TASK_PERIOD_MS) {
        led_status_anim_chase(COLOR_BOOT_WHITE, t, LED_STATUS_BOOT_SWEEP_STEP_MS, handle->led_count, pixels);
        render_pixels(handle, pixels);
        vTaskDelay(pdMS_TO_TICKS(LED_STATUS_TASK_PERIOD_MS));
    }
    render_solid(handle, COLOR_OFF);
}

static void run_blink_for(led_status_indicator_handle_t handle, led_status_rgb_t color, uint32_t on_ms,
                          uint32_t off_ms, uint32_t total_ms)
{
    for (uint32_t t = 0; t < total_ms && handle->running; t += LED_STATUS_TASK_PERIOD_MS) {
        render_solid(handle, led_status_anim_blink(color, t, on_ms, off_ms));
        vTaskDelay(pdMS_TO_TICKS(LED_STATUS_TASK_PERIOD_MS));
    }
}

static void run_flash_n_times(led_status_indicator_handle_t handle, led_status_rgb_t color, uint32_t on_ms,
                              uint32_t off_ms, uint32_t count)
{
    for (uint32_t i = 0; i < count && handle->running; i++) {
        render_solid(handle, color);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        render_solid(handle, COLOR_OFF);
        if (off_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(off_ms));
        }
    }
}

static void run_one_shot_event(led_status_indicator_handle_t handle, led_status_queue_event_t event)
{
    switch (event) {
    case LED_STATUS_QEVT_ERROR:
        run_blink_for(handle, COLOR_ERROR_RED, LED_STATUS_ERROR_BLINK_ON_MS, LED_STATUS_ERROR_BLINK_OFF_MS,
                      LED_STATUS_ERROR_DURATION_MS);
        break;
    case LED_STATUS_QEVT_MESSAGE:
        run_flash_n_times(handle, COLOR_MESSAGE_WHITE, LED_STATUS_MESSAGE_FLASH_ON_MS, LED_STATUS_MESSAGE_FLASH_OFF_MS,
                          LED_STATUS_MESSAGE_FLASH_COUNT);
        break;
    case LED_STATUS_QEVT_CONFIG_SAVED:
        run_flash_n_times(handle, COLOR_CONFIG_SAVED_GREEN, LED_STATUS_CONFIG_SAVED_FLASH_MS, 0, 1);
        break;
    case LED_STATUS_QEVT_WIFI_CONNECTED_FLASH:
        run_flash_n_times(handle, COLOR_WIFI_CONNECTED_GREEN, LED_STATUS_WIFI_CONNECTED_FLASH_MS, 0, 1);
        break;
    }
}

static void led_status_task(void *arg)
{
    led_status_indicator_handle_t handle = (led_status_indicator_handle_t)arg;

    run_boot_animation(handle);

    led_status_bg_mode_t last_mode = LED_STATUS_MODE_IDLE;
    int64_t mode_enter_us = esp_timer_get_time();

    while (handle->running) {
        led_status_queue_event_t queued_event;
        if (xQueueReceive(handle->event_queue, &queued_event, 0) == pdTRUE) {
            run_one_shot_event(handle, queued_event);
            mode_enter_us = esp_timer_get_time();
            continue;
        }

        wifi_manager_status_t wifi_status = {0};
        wifi_manager_get_status(&wifi_status);

        claw_core_agent_loop_phase_t phase = CLAW_CORE_AGENT_LOOP_PHASE_IDLE;
        claw_core_handle_t core = app_claw_get_core();
        if (core) {
            phase = claw_core_get_agent_loop_phase(core);
        }

        if (wifi_status.sta_connected && !handle->wifi_ever_connected) {
            handle->wifi_ever_connected = true;
            led_status_queue_event_t connected_event = LED_STATUS_QEVT_WIFI_CONNECTED_FLASH;
            xQueueSend(handle->event_queue, &connected_event, 0);
        }

        led_status_bg_mode_t mode = arbitrate_mode(handle, &wifi_status, phase);
        if (mode != last_mode) {
            mode_enter_us = esp_timer_get_time();
            last_mode = mode;
        }

        uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - mode_enter_us) / 1000);
        render_mode(handle, mode, elapsed_ms);

        vTaskDelay(pdMS_TO_TICKS(LED_STATUS_TASK_PERIOD_MS));
    }

    render_solid(handle, COLOR_OFF);
    handle->task = NULL;
    vTaskDelete(NULL);
}

esp_err_t led_status_indicator_create(const led_status_indicator_config_t *config,
                                       led_status_indicator_handle_t *ret_handle)
{
    ESP_RETURN_ON_FALSE(config && ret_handle, ESP_ERR_INVALID_ARG, TAG, "config/ret_handle required");
    ESP_RETURN_ON_FALSE(config->led_count >= 1 && config->led_count <= LED_STATUS_MAX_LEDS, ESP_ERR_INVALID_ARG, TAG,
                        "led_count must be between 1 and %d", LED_STATUS_MAX_LEDS);

    led_status_indicator_handle_t handle = calloc(1, sizeof(*handle));
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_NO_MEM, TAG, "no memory for handle");

    handle->gpio_num = config->gpio_num;
    handle->led_count = config->led_count;
    handle->max_brightness = config->max_brightness;

    led_strip_config_t strip_config = {
        .strip_gpio_num = config->gpio_num,
        .max_leds = config->led_count,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LED_STATUS_RMT_RESOLUTION_HZ,
        .mem_block_symbols = 0,
        .flags = {
            .with_dma = 0,
        },
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &handle->strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        free(handle);
        return err;
    }

    handle->event_queue = xQueueCreate(LED_STATUS_EVENT_QUEUE_LEN, sizeof(led_status_queue_event_t));
    if (!handle->event_queue) {
        led_strip_del(handle->strip);
        free(handle);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "initialized, gpio=%d leds=%u max_brightness=%u", handle->gpio_num, handle->led_count,
             handle->max_brightness);
    *ret_handle = handle;
    return ESP_OK;
}

esp_err_t led_status_indicator_start(led_status_indicator_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "handle required");
    ESP_RETURN_ON_FALSE(!handle->task, ESP_ERR_INVALID_STATE, TAG, "already started");

    handle->running = true;
    BaseType_t ok = xTaskCreate(led_status_task, "led_status", LED_STATUS_TASK_STACK_SIZE, handle,
                                LED_STATUS_TASK_PRIORITY, &handle->task);
    if (ok != pdPASS) {
        handle->running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t led_status_indicator_stop(led_status_indicator_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "handle required");
    handle->running = false;
    while (handle->task) {
        vTaskDelay(pdMS_TO_TICKS(LED_STATUS_TASK_PERIOD_MS));
    }
    return ESP_OK;
}

esp_err_t led_status_indicator_delete(led_status_indicator_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "handle required");
    if (handle->task) {
        led_status_indicator_stop(handle);
    }
    if (handle->event_queue) {
        vQueueDelete(handle->event_queue);
    }
    if (handle->strip) {
        led_strip_del(handle->strip);
    }
    free(handle);
    return ESP_OK;
}

esp_err_t led_status_indicator_notify(led_status_indicator_handle_t handle, led_status_indicator_event_t event)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "handle required");
    led_status_queue_event_t queued_event = (led_status_queue_event_t)event;
    if (xQueueSend(handle->event_queue, &queued_event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropping event %d", (int)event);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
