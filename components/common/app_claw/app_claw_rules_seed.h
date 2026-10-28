/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/**
 * Copies chat-command rules that exist in the SYSTEM recovery seed but are
 * missing from the DATA router rules file. Rules already present (possibly
 * customized by the user) are never modified, and only rules listed in the
 * implementation's id table are considered, so rules the user deleted on
 * purpose are not resurrected.
 */
void app_claw_router_rules_merge_seed(const char *rules_path);
