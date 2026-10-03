/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cap_exec_state.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"
#include "unity_test_runner.h"
#include "wear_levelling.h"

#define TEST_BASE_PATH  "/tmp"
#define TEST_STATE_DIR  TEST_BASE_PATH "/state"
#define TEST_STATE_PATH TEST_STATE_DIR "/exec_state.json"
#define TEST_TMP_PATH   TEST_STATE_PATH ".tmp"
#define TEST_BAD_PATH   TEST_STATE_PATH ".bad"
#define TEST_JSON_BUF   2048

static wl_handle_t s_wl = WL_INVALID_HANDLE;

static void ensure_fs(void)
{
    esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = 4096,
    };

    if (s_wl == WL_INVALID_HANDLE) {
        TEST_ESP_OK(esp_vfs_fat_spiflash_mount_rw_wl(TEST_BASE_PATH, "storage", &cfg, &s_wl));
    }
    mkdir(TEST_STATE_DIR, 0775);
    unlink(TEST_STATE_PATH);
    unlink(TEST_TMP_PATH);
    unlink(TEST_BAD_PATH);
}

static cap_exec_state_handle_t open_store(uint32_t max_entries, uint32_t max_bytes, int32_t delay_ms)
{
    cap_exec_state_config_t cfg = CAP_EXEC_STATE_CONFIG_DEFAULT();
    cap_exec_state_handle_t h = NULL;

    cfg.path = TEST_STATE_PATH;
    cfg.max_entries = max_entries;
    cfg.max_file_bytes = max_bytes;
    cfg.flush_delay_ms = delay_ms;
    TEST_ESP_OK(cap_exec_state_create(&cfg, &h));
    TEST_ASSERT_NOT_NULL(h);
    return h;
}

static void write_file(const char *path, const char *text)
{
    FILE *fp = fopen(path, "wb");

    TEST_ASSERT_NOT_NULL(fp);
    fputs(text, fp);
    fclose(fp);
}

static bool file_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

static void mark_key(cap_exec_state_handle_t h, int i)
{
    char key[16];

    snprintf(key, sizeof(key), "k%d", i);
    TEST_ESP_OK(cap_exec_state_mark(h, key, "t", CAP_EXEC_STATE_STATUS_COMPLETED, "s"));
}

static bool has_key(cap_exec_state_handle_t h, const char *key)
{
    return cap_exec_state_check(h, key, NULL) == ESP_OK;
}

TEST_CASE("T1 mark new entry", "[exec_state]")
{
    cap_exec_state_entry_t e;
    cap_exec_state_handle_t h;

    ensure_fs();
    h = open_store(10, 8192, 0);
    TEST_ESP_OK(cap_exec_state_mark(h, "email:1", "email", CAP_EXEC_STATE_STATUS_COMPLETED, "ok"));
    TEST_ESP_OK(cap_exec_state_check(h, "email:1", &e));
    TEST_ASSERT_EQUAL_UINT32(1, e.run_count);
    TEST_ASSERT_EQUAL_INT64(e.first_seen_ms, e.last_run_ms);
    TEST_ASSERT_EQUAL_STRING("ok", e.summary);
    cap_exec_state_delete(h);
}

TEST_CASE("T2 repeated mark keeps first_seen", "[exec_state]")
{
    cap_exec_state_entry_t first, second;
    cap_exec_state_handle_t h;

    ensure_fs();
    h = open_store(10, 8192, 0);
    mark_key(h, 1);
    TEST_ESP_OK(cap_exec_state_check(h, "k1", &first));
    vTaskDelay(pdMS_TO_TICKS(5));
    mark_key(h, 1);
    TEST_ESP_OK(cap_exec_state_check(h, "k1", &second));
    TEST_ASSERT_EQUAL_UINT32(2, second.run_count);
    TEST_ASSERT_EQUAL_INT64(first.first_seen_ms, second.first_seen_ms);
    TEST_ASSERT_TRUE(second.last_run_ms >= first.last_run_ms);
    cap_exec_state_delete(h);
}

TEST_CASE("T3 failed then completed", "[exec_state]")
{
    cap_exec_state_entry_t e;
    cap_exec_state_handle_t h;

    ensure_fs();
    h = open_store(10, 8192, 0);
    TEST_ESP_OK(cap_exec_state_mark(h, "k", "t", CAP_EXEC_STATE_STATUS_FAILED, "boom"));
    TEST_ESP_OK(cap_exec_state_mark(h, "k", "t", CAP_EXEC_STATE_STATUS_COMPLETED, "fixed"));
    TEST_ESP_OK(cap_exec_state_check(h, "k", &e));
    TEST_ASSERT_EQUAL(CAP_EXEC_STATE_STATUS_COMPLETED, e.status);
    TEST_ASSERT_EQUAL_UINT32(2, e.run_count);
    cap_exec_state_delete(h);
}

TEST_CASE("T4 check missing key", "[exec_state]")
{
    cap_exec_state_handle_t h;

    ensure_fs();
    h = open_store(10, 8192, 0);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, cap_exec_state_check(h, "nope", NULL));
    cap_exec_state_delete(h);
}

TEST_CASE("T5 eviction by count", "[exec_state]")
{
    cap_exec_state_handle_t h;

    ensure_fs();
    h = open_store(10, 8192, 0);
    for (int i = 0; i <= 10; i++) {
        mark_key(h, i);
    }
    TEST_ASSERT_EQUAL(10, cap_exec_state_count(h));
    TEST_ASSERT_FALSE(has_key(h, "k0"));
    TEST_ASSERT_TRUE(has_key(h, "k1"));
    TEST_ASSERT_TRUE(has_key(h, "k10"));
    cap_exec_state_delete(h);
}

TEST_CASE("T6 re-mark saves oldest from eviction", "[exec_state]")
{
    cap_exec_state_handle_t h;

    ensure_fs();
    h = open_store(10, 8192, 0);
    for (int i = 0; i < 10; i++) {
        mark_key(h, i);
    }
    mark_key(h, 0);
    mark_key(h, 10);
    TEST_ASSERT_TRUE(has_key(h, "k0"));
    TEST_ASSERT_FALSE(has_key(h, "k1"));
    cap_exec_state_delete(h);
}

TEST_CASE("T7 check does not reorder", "[exec_state]")
{
    cap_exec_state_handle_t h;

    ensure_fs();
    h = open_store(10, 8192, 0);
    for (int i = 0; i < 10; i++) {
        mark_key(h, i);
    }
    TEST_ESP_OK(cap_exec_state_check(h, "k0", NULL));
    mark_key(h, 10);
    TEST_ASSERT_FALSE(has_key(h, "k0"));
    cap_exec_state_delete(h);
}

TEST_CASE("T8 byte budget", "[exec_state]")
{
    char summary[CAP_EXEC_STATE_SUMMARY_MAX_LEN + 1];
    struct stat st;
    cap_exec_state_handle_t h;

    ensure_fs();
    memset(summary, 's', sizeof(summary) - 1);
    summary[sizeof(summary) - 1] = '\0';
    h = open_store(100, 8192, 0);
    for (int i = 0; i < 60; i++) {
        char key[16];

        snprintf(key, sizeof(key), "k%d", i);
        TEST_ESP_OK(cap_exec_state_mark(h, key, "t", CAP_EXEC_STATE_STATUS_COMPLETED, summary));
    }
    TEST_ASSERT_EQUAL(0, stat(TEST_STATE_PATH, &st));
    TEST_ASSERT_TRUE(st.st_size <= 8192);
    TEST_ASSERT_TRUE(cap_exec_state_count(h) < 60);
    TEST_ASSERT_TRUE(has_key(h, "k59"));
    cap_exec_state_delete(h);
}

TEST_CASE("T9 state survives delete and create", "[exec_state]")
{
    cap_exec_state_entry_t e;
    cap_exec_state_handle_t h;

    ensure_fs();
    h = open_store(10, 8192, 2000);
    TEST_ESP_OK(cap_exec_state_mark(h, "persist", "t", CAP_EXEC_STATE_STATUS_SKIPPED, "kept"));
    TEST_ESP_OK(cap_exec_state_delete(h));
    h = open_store(10, 8192, 2000);
    TEST_ESP_OK(cap_exec_state_check(h, "persist", &e));
    TEST_ASSERT_EQUAL(CAP_EXEC_STATE_STATUS_SKIPPED, e.status);
    TEST_ASSERT_EQUAL_STRING("kept", e.summary);
    cap_exec_state_delete(h);
}

TEST_CASE("T10 missing file starts empty", "[exec_state]")
{
    cap_exec_state_handle_t h;

    ensure_fs();
    h = open_store(10, 8192, 0);
    TEST_ASSERT_EQUAL(0, cap_exec_state_count(h));
    cap_exec_state_delete(h);
}

TEST_CASE("T11 corrupt file moves to .bad", "[exec_state]")
{
    cap_exec_state_handle_t h;

    ensure_fs();
    write_file(TEST_STATE_PATH, "{not json");
    h = open_store(10, 8192, 0);
    TEST_ASSERT_EQUAL(0, cap_exec_state_count(h));
    TEST_ASSERT_TRUE(file_exists(TEST_BAD_PATH));
    cap_exec_state_delete(h);
}

TEST_CASE("T12 future version moves to .bad", "[exec_state]")
{
    cap_exec_state_handle_t h;

    ensure_fs();
    write_file(TEST_STATE_PATH, "{\"version\":99,\"entries\":[{\"key\":\"a\"}]}");
    h = open_store(10, 8192, 0);
    TEST_ASSERT_EQUAL(0, cap_exec_state_count(h));
    TEST_ASSERT_TRUE(file_exists(TEST_BAD_PATH));
    cap_exec_state_delete(h);
}

TEST_CASE("T13 missing and unknown fields", "[exec_state]")
{
    cap_exec_state_entry_t e;
    cap_exec_state_handle_t h;

    ensure_fs();
    write_file(TEST_STATE_PATH,
               "{\"version\":1,\"entries\":[{\"key\":\"a\"},"
               "{\"key\":\"b\",\"status\":\"zzz\",\"extra\":1,\"run_count\":5},"
               "{\"nokey\":1},{\"key\":\"a\",\"kind\":\"x\"}]}");
    h = open_store(10, 8192, 0);
    TEST_ASSERT_EQUAL(2, cap_exec_state_count(h));
    TEST_ESP_OK(cap_exec_state_check(h, "b", &e));
    TEST_ASSERT_EQUAL(CAP_EXEC_STATE_STATUS_FAILED, e.status);
    TEST_ASSERT_EQUAL_UINT32(5, e.run_count);
    TEST_ESP_OK(cap_exec_state_check(h, "a", &e));
    TEST_ASSERT_EQUAL_STRING("x", e.kind);
    TEST_ASSERT_EQUAL_UINT32(1, e.run_count);
    cap_exec_state_delete(h);
}

TEST_CASE("T14 orphan tmp file", "[exec_state]")
{
    cap_exec_state_handle_t h;

    ensure_fs();
    write_file(TEST_STATE_PATH, "{\"version\":1,\"entries\":[{\"key\":\"k1\"}]}");
    write_file(TEST_TMP_PATH, "garbage");
    h = open_store(10, 8192, 0);
    TEST_ASSERT_TRUE(has_key(h, "k1"));
    TEST_ASSERT_FALSE(file_exists(TEST_TMP_PATH));
    cap_exec_state_delete(h);

    /* Crash between unlink(path) and rename(tmp, path): only a valid tmp remains. */
    unlink(TEST_STATE_PATH);
    write_file(TEST_TMP_PATH, "{\"version\":1,\"entries\":[{\"key\":\"k2\"}]}");
    h = open_store(10, 8192, 0);
    TEST_ASSERT_TRUE(has_key(h, "k2"));
    cap_exec_state_delete(h);
}

TEST_CASE("T15 invalid arguments", "[exec_state]")
{
    char long_key[CAP_EXEC_STATE_KEY_MAX_LEN + 2];
    cap_exec_state_handle_t h;

    ensure_fs();
    h = open_store(10, 8192, 0);
    memset(long_key, 'x', sizeof(long_key) - 1);
    long_key[sizeof(long_key) - 1] = '\0';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cap_exec_state_mark(h, "", "t", CAP_EXEC_STATE_STATUS_COMPLETED, ""));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cap_exec_state_mark(h, NULL, "t", CAP_EXEC_STATE_STATUS_COMPLETED, ""));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cap_exec_state_mark(h, long_key, "t", CAP_EXEC_STATE_STATUS_COMPLETED, ""));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cap_exec_state_mark(h, "k", "t", CAP_EXEC_STATE_STATUS_MAX, ""));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cap_exec_state_mark(h, "k", "t", (cap_exec_state_status_t) -1, ""));
    TEST_ASSERT_EQUAL(0, cap_exec_state_count(h));
    cap_exec_state_delete(h);
}

TEST_CASE("T16 UTF-8 safe truncation", "[exec_state]")
{
    char summary[CAP_EXEC_STATE_SUMMARY_MAX_LEN + 8];
    char kind[31];
    cap_exec_state_entry_t e;
    cap_exec_state_handle_t h;

    ensure_fs();
    h = open_store(10, 8192, 0);
    memset(summary, 'a', 199);
    strcpy(summary + 199, "\xC3\xA9" "tail");
    memset(kind, 'k', 30);
    kind[30] = '\0';
    TEST_ESP_OK(cap_exec_state_mark(h, "u", kind, CAP_EXEC_STATE_STATUS_COMPLETED, summary));
    TEST_ESP_OK(cap_exec_state_check(h, "u", &e));
    TEST_ASSERT_EQUAL(199, strlen(e.summary));
    TEST_ASSERT_EQUAL(CAP_EXEC_STATE_KIND_MAX_LEN, strlen(e.kind));
    cap_exec_state_delete(h);
}

TEST_CASE("T17 list order and limit", "[exec_state]")
{
    char buf[TEST_JSON_BUF];
    cap_exec_state_handle_t h;
    const char *k4, *k3, *k2;

    ensure_fs();
    h = open_store(10, 8192, 0);
    for (int i = 0; i < 5; i++) {
        mark_key(h, i);
    }
    TEST_ESP_OK(cap_exec_state_list_json(h, 3, buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"count\":3"));
    k4 = strstr(buf, "\"k4\"");
    k3 = strstr(buf, "\"k3\"");
    k2 = strstr(buf, "\"k2\"");
    TEST_ASSERT_TRUE(k4 && k3 && k2 && k4 < k3 && k3 < k2);
    TEST_ASSERT_NULL(strstr(buf, "\"k1\""));
    TEST_ESP_OK(cap_exec_state_list_json(h, 0, buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"count\":5"));
    TEST_ASSERT_NULL(strstr(buf, "truncated"));
    cap_exec_state_delete(h);
}

TEST_CASE("T18 list with small buffer", "[exec_state]")
{
    uint8_t raw[400 + 16];
    char *buf = (char *)raw;
    cap_exec_state_handle_t h;

    ensure_fs();
    h = open_store(10, 8192, 0);
    for (int i = 0; i < 5; i++) {
        mark_key(h, i);
    }
    memset(raw, 0xA5, sizeof(raw));
    TEST_ESP_OK(cap_exec_state_list_json(h, 0, buf, 400));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"truncated\":true"));
    TEST_ASSERT_TRUE(strlen(buf) < 400);
    for (int i = 400; i < (int)sizeof(raw); i++) {
        TEST_ASSERT_EQUAL_HEX8(0xA5, raw[i]);
    }

    memset(raw, 0xA5, sizeof(raw));
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, cap_exec_state_list_json(h, 0, buf, 8));
    for (int i = 8; i < (int)sizeof(raw); i++) {
        TEST_ASSERT_EQUAL_HEX8(0xA5, raw[i]);
    }
    cap_exec_state_delete(h);
}

TEST_CASE("T19 coalesced deferred flush", "[exec_state]")
{
    char text[1024] = {0};
    cap_exec_state_handle_t h;
    FILE *fp;
    int keys = 0;

    ensure_fs();
    h = open_store(20, 8192, 200);
    TEST_ESP_OK(cap_exec_state_start(h));
    for (int i = 0; i < 10; i++) {
        mark_key(h, i);
    }
    TEST_ASSERT_FALSE(file_exists(TEST_STATE_PATH));
    vTaskDelay(pdMS_TO_TICKS(500));
    fp = fopen(TEST_STATE_PATH, "rb");
    TEST_ASSERT_NOT_NULL(fp);
    fread(text, 1, sizeof(text) - 1, fp);
    fclose(fp);
    for (const char *p = text; (p = strstr(p, "\"key\"")) != NULL; p++) {
        keys++;
    }
    TEST_ASSERT_EQUAL(10, keys);
    TEST_ESP_OK(cap_exec_state_stop(h));
    cap_exec_state_delete(h);
}

TEST_CASE("T20 run_count saturates", "[exec_state]")
{
    cap_exec_state_entry_t e;
    cap_exec_state_handle_t h;

    ensure_fs();
    write_file(TEST_STATE_PATH, "{\"version\":1,\"entries\":[{\"key\":\"s\",\"run_count\":4294967295}]}");
    h = open_store(10, 8192, 0);
    TEST_ESP_OK(cap_exec_state_mark(h, "s", "t", CAP_EXEC_STATE_STATUS_COMPLETED, ""));
    TEST_ESP_OK(cap_exec_state_check(h, "s", &e));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, e.run_count);
    cap_exec_state_delete(h);
}

void app_main(void)
{
    unity_run_menu();
}
