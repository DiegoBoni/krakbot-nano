/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "esp_log.h"
#include "mbedtls/base64.h"

static const char *TAG = "http_server_auth";

#define HTTP_SERVER_AUTH_ADMIN_USER "admin"

static bool constant_time_streq(const char *a, const char *b)
{
    size_t len_a = strlen(a);
    size_t len_b = strlen(b);
    if (len_a != len_b) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < len_a; i++) {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static esp_err_t send_401(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"KrakBot Nano\"");
    httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
}

esp_err_t http_server_require_auth(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();

    char hdr[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return send_401(req);
    }

    static const char prefix[] = "Basic ";
    size_t prefix_len = sizeof(prefix) - 1;
    if (strncmp(hdr, prefix, prefix_len) != 0) {
        return send_401(req);
    }

    unsigned char decoded[96] = {0};
    size_t decoded_len = 0;
    int ret = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                                    (const unsigned char *)(hdr + prefix_len),
                                    strlen(hdr + prefix_len));
    if (ret != 0) {
        return send_401(req);
    }
    decoded[decoded_len] = '\0';

    char *colon = strchr((char *)decoded, ':');
    if (!colon) {
        return send_401(req);
    }
    *colon = '\0';
    const char *user = (const char *)decoded;
    const char *pass = colon + 1;

    app_config_t *config = calloc(1, sizeof(*config));
    if (!config) {
        return send_401(req);
    }
    if (!ctx->services.load_config || ctx->services.load_config(config) != ESP_OK) {
        free(config);
        return send_401(req);
    }

    bool ok = constant_time_streq(user, HTTP_SERVER_AUTH_ADMIN_USER) &&
              constant_time_streq(pass, config->admin_password);
    free(config);

    if (!ok) {
        ESP_LOGW(TAG, "Auth failed for %s", req->uri);
        return send_401(req);
    }

    return ESP_OK;
}
