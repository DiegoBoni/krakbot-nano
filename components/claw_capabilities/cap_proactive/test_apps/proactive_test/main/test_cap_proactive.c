/* SPDX-License-Identifier: CC0-1.0 */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include "sdkconfig.h"
#include "cap_proactive.h"
#include "cap_scheduler.h"
#include "claw_event_router.h"
#include "claw_cap.h"
#include "claw_paths.h"
#include "esp_vfs_fat.h"
#include "nvs_flash.h"
#include "unity.h"
#include "unity_test_runner.h"
#include "wear_levelling.h"

#define TEST_MOUNT "/tmp"
static wl_handle_t s_wl = WL_INVALID_HANDLE;
static cap_proactive_handle_t s_handle;
static char s_output[1024];
static char s_router_path[128];
static char s_scheduler_path[128];

static void fixture(void)
{
    if (!s_handle) {
        TEST_ESP_OK(nvs_flash_init());
        esp_vfs_fat_mount_config_t mount = {
            .format_if_mount_failed = true, .max_files = 4, .allocation_unit_size = 4096,
        };
        TEST_ESP_OK(esp_vfs_fat_spiflash_mount_rw_wl(TEST_MOUNT, "storage", &mount, &s_wl));
        TEST_ESP_OK(claw_paths_set(CLAW_PATH_DATA, TEST_MOUNT));
        TEST_ESP_OK(claw_paths_join(CLAW_PATH_DATA, "pulse_rules.json", s_router_path, sizeof(s_router_path)));
        TEST_ESP_OK(claw_paths_join(CLAW_PATH_DATA, "pulse_schedules.json", s_scheduler_path, sizeof(s_scheduler_path)));
        FILE *fp = fopen(s_router_path, "w");
        TEST_ASSERT_NOT_NULL(fp);
        /* Router canonical persistence format is a JSON array. */
        fputs("[]", fp);
        fclose(fp);
        claw_event_router_config_t *router = calloc(1, sizeof(*router));
        TEST_ASSERT_NOT_NULL(router);
        router->rules_path = s_router_path;
        router->max_rules = 1; /* deterministic rule-capacity failure injection */
        TEST_ESP_OK(claw_event_router_init(router));
        free(router);
        cap_scheduler_config_t scheduler = {
            .schedules_path = s_scheduler_path,
            .max_items = 4, .publish_event = claw_event_router_publish,
        };
        TEST_ESP_OK(cap_scheduler_init(&scheduler));
        TEST_ESP_OK(claw_cap_init());
        cap_proactive_config_t config = { .scheduler_available = true, .router_available = true };
        TEST_ESP_OK(cap_proactive_create(&config, &s_handle));
        TEST_ESP_OK(cap_proactive_register_group(s_handle));
    }
    esp_err_t err = cap_scheduler_remove(CAP_PROACTIVE_ITEM_ID);
    TEST_ASSERT_TRUE(err == ESP_OK || err == ESP_ERR_NOT_FOUND);
    err = claw_event_router_delete_rule(CAP_PROACTIVE_RULE_ID);
    TEST_ASSERT_TRUE(err == ESP_OK || err == ESP_ERR_NOT_FOUND);
    err = claw_event_router_delete_rule("occupier");
    TEST_ASSERT_TRUE(err == ESP_OK || err == ESP_ERR_NOT_FOUND);
}

TEST_CASE("T1-T4 interval boundaries and invalid arguments", "[proactive]")
{
    uint32_t applied;
    bool clamped;
    TEST_ESP_OK(cap_proactive_clamp_interval(CONFIG_CAP_PROACTIVE_MIN_INTERVAL_SEC - 1, &applied, &clamped));
    TEST_ASSERT_EQUAL_UINT32(CONFIG_CAP_PROACTIVE_MIN_INTERVAL_SEC, applied);
    TEST_ASSERT_TRUE(clamped);
    TEST_ESP_OK(cap_proactive_clamp_interval(CONFIG_CAP_PROACTIVE_MIN_INTERVAL_SEC, &applied, &clamped));
    TEST_ASSERT_FALSE(clamped);
    TEST_ESP_OK(cap_proactive_clamp_interval(CONFIG_CAP_PROACTIVE_MAX_INTERVAL_SEC + 1, &applied, &clamped));
    TEST_ASSERT_EQUAL_UINT32(CONFIG_CAP_PROACTIVE_MAX_INTERVAL_SEC, applied);
    TEST_ASSERT_TRUE(clamped);
    TEST_ESP_OK(cap_proactive_clamp_interval(CONFIG_CAP_PROACTIVE_MAX_INTERVAL_SEC, &applied, &clamped));
    TEST_ASSERT_FALSE(clamped);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cap_proactive_clamp_interval(0, &applied, &clamped));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cap_proactive_clamp_interval(-1, &applied, &clamped));
}

TEST_CASE("T5 handler rejects absent nonnumeric fractional overflow seconds", "[proactive]")
{
    fixture();
    const claw_cap_descriptor_t *descriptor = claw_cap_find("proactive_set_interval");
    TEST_ASSERT_NOT_NULL(descriptor);
    const char *inputs[] = {"{}", "{\"seconds\":\"abc\"}", "{\"seconds\":2147483648}",
                            "{\"seconds\":1.5}", "{\"seconds\":0}", NULL};
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, descriptor->execute(inputs[i], NULL, s_output, sizeof(s_output)));
    }
}

TEST_CASE("T6-T10 OFF default idempotency preserved interval and actual status", "[proactive]")
{
    fixture();
    cap_proactive_status_t status;
    cap_proactive_rule_result_t rule;
    uint32_t applied;
    bool clamped;
    TEST_ESP_OK(cap_proactive_start(s_handle));
    TEST_ESP_OK(cap_proactive_get_status(s_handle, &status));
    TEST_ASSERT_FALSE(status.enabled);
    TEST_ASSERT_FALSE(status.item_present);
    TEST_ASSERT_EQUAL_UINT32(CONFIG_CAP_PROACTIVE_DEFAULT_INTERVAL_SEC, status.interval_sec);
    TEST_ESP_OK(cap_proactive_enable(s_handle, &rule));
    TEST_ASSERT_EQUAL(CAP_PROACTIVE_RULE_CREATED, rule);
    TEST_ESP_OK(cap_proactive_enable(s_handle, &rule));
    TEST_ASSERT_EQUAL(CAP_PROACTIVE_RULE_EXISTING, rule);
    TEST_ESP_OK(cap_proactive_get_status(s_handle, &status));
    TEST_ASSERT_TRUE(status.enabled);
    TEST_ASSERT_TRUE(status.rule_present);
    TEST_ESP_OK(cap_proactive_set_interval(s_handle, CONFIG_CAP_PROACTIVE_MAX_INTERVAL_SEC + 1, &applied, &clamped));
    TEST_ESP_OK(cap_proactive_get_status(s_handle, &status));
    TEST_ASSERT_TRUE(status.enabled);
    TEST_ASSERT_TRUE(clamped);
    TEST_ASSERT_EQUAL_UINT32(applied, status.interval_sec);
    TEST_ESP_OK(cap_proactive_disable(s_handle));
    TEST_ESP_OK(cap_proactive_disable(s_handle));
    TEST_ESP_OK(cap_proactive_get_status(s_handle, &status));
    TEST_ASSERT_FALSE(status.enabled);
    TEST_ASSERT_TRUE(status.item_present);
    TEST_ASSERT_TRUE(status.rule_present);
    TEST_ESP_OK(cap_scheduler_reload());
    TEST_ESP_OK(cap_proactive_get_status(s_handle, &status));
    TEST_ASSERT_FALSE(status.enabled);
    TEST_ASSERT_EQUAL_UINT32(applied, status.interval_sec);
    TEST_ESP_OK(cap_proactive_enable(s_handle, &rule));
    TEST_ASSERT_EQUAL(CAP_PROACTIVE_RULE_EXISTING, rule);
    TEST_ESP_OK(claw_event_router_delete_rule(CAP_PROACTIVE_RULE_ID));
    TEST_ESP_OK(cap_proactive_start(s_handle));
    TEST_ESP_OK(cap_proactive_get_status(s_handle, &status));
    TEST_ASSERT_TRUE(status.enabled);
    TEST_ASSERT_TRUE(status.rule_present);
}

TEST_CASE("T11 rollback on router capacity failure preserves preexisting OFF item", "[proactive]")
{
    fixture();
    TEST_ESP_OK(claw_event_router_add_rule_json("{\"id\":\"occupier\",\"match\":{\"event_type\":\"unused\"},\"actions\":[{\"type\":\"drop\"}]}"));
    cap_proactive_rule_result_t result;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, cap_proactive_enable(s_handle, &result));
    cap_proactive_status_t status;
    TEST_ESP_OK(cap_proactive_get_status(s_handle, &status));
    TEST_ASSERT_FALSE(status.item_present);
    uint32_t applied;
    bool clamped;
    TEST_ESP_OK(cap_proactive_set_interval(s_handle, CONFIG_CAP_PROACTIVE_DEFAULT_INTERVAL_SEC, &applied, &clamped));
    TEST_ASSERT_NOT_EQUAL(ESP_OK, cap_proactive_enable(s_handle, &result));
    TEST_ESP_OK(cap_proactive_get_status(s_handle, &status));
    TEST_ASSERT_TRUE(status.item_present);
    TEST_ASSERT_FALSE(status.enabled);
    TEST_ASSERT_FALSE(status.rule_present);
}

TEST_CASE("T12 incompatible enabled item is disabled at startup", "[proactive]")
{
    fixture();
    cap_scheduler_item_t *item = calloc(1, sizeof(*item));
    TEST_ASSERT_NOT_NULL(item);
    snprintf(item->id, sizeof(item->id), "%s", CAP_PROACTIVE_ITEM_ID);
    item->kind = CAP_SCHEDULER_ITEM_INTERVAL;
    item->interval_ms = 1;
    item->enabled = true;
    TEST_ESP_OK(cap_scheduler_add(item));
    free(item);
    TEST_ESP_OK(cap_proactive_start(s_handle));
    cap_proactive_status_t status;
    TEST_ESP_OK(cap_proactive_get_status(s_handle, &status));
    TEST_ASSERT_FALSE(status.enabled);
    TEST_ASSERT_FALSE(status.rule_present);
}

TEST_CASE("corrupt router file repaired and interval changes while OFF stay OFF", "[proactive]")
{
    fixture();
    uint32_t applied;
    bool clamped;
    TEST_ESP_OK(cap_proactive_set_interval(s_handle, CONFIG_CAP_PROACTIVE_MIN_INTERVAL_SEC, &applied, &clamped));
    cap_proactive_status_t status;
    TEST_ESP_OK(cap_proactive_get_status(s_handle, &status));
    TEST_ASSERT_TRUE(status.item_present);
    TEST_ASSERT_FALSE(status.enabled);
    TEST_ASSERT_FALSE(status.rule_present);
    cap_proactive_rule_result_t result;
    TEST_ESP_OK(cap_proactive_enable(s_handle, &result));
    FILE *fp = fopen(s_router_path, "w");
    TEST_ASSERT_NOT_NULL(fp);
    fputs("{broken", fp);
    fclose(fp);
    TEST_ESP_OK(cap_proactive_start(s_handle));
    TEST_ESP_OK(cap_proactive_get_status(s_handle, &status));
    TEST_ASSERT_TRUE(status.enabled);
    TEST_ASSERT_TRUE(status.rule_present);
    TEST_ASSERT_EQUAL_UINT32(applied, status.interval_sec);
    TEST_ESP_OK(cap_proactive_enable(s_handle, &result));
    TEST_ASSERT_EQUAL(CAP_PROACTIVE_RULE_EXISTING, result);
}

TEST_CASE("enable repairs mismatched owned rule", "[proactive]")
{
    fixture();
    TEST_ESP_OK(claw_event_router_add_rule_json(
        "{\"id\":\"" CAP_PROACTIVE_RULE_ID "\",\"enabled\":false,"
        "\"match\":{\"event_type\":\"wrong\"},\"actions\":[{\"type\":\"drop\"}]}"));
    cap_proactive_rule_result_t result;
    TEST_ESP_OK(cap_proactive_enable(s_handle, &result));
    TEST_ASSERT_EQUAL(CAP_PROACTIVE_RULE_UPDATED, result);
    TEST_ESP_OK(cap_proactive_enable(s_handle, &result));
    TEST_ASSERT_EQUAL(CAP_PROACTIVE_RULE_EXISTING, result);
}

void app_main(void)
{
    unity_run_menu();
}
