/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

/*
 * The config API is reachable only from devices on the same Wi-Fi network as
 * this device (station network or the device's own AP), which is already
 * password-protected. There is no separate credential gate on top of that.
 */
esp_err_t http_server_require_auth(httpd_req_t *req)
{
    (void)req;
    return ESP_OK;
}
