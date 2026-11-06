/*
 * SPDX-FileCopyrightText: 2026 Dbb
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the bridge for the role selected at compile time.
 *
 * The call registers the USB callback table (wired and wireless host builds
 * only), brings up the transport of the compiled role -- the target UART and
 * the BOOT/RST lines for the roles that own them, the ESP-NOW link for the
 * wireless roles -- and starts the tasks that move data afterwards.
 *
 * Call it exactly once from app_main(), after status_led_init().
 *
 * @return ESP_OK on success, an error otherwise.
 */
esp_err_t bridge_start(void);

#ifdef __cplusplus
}
#endif
