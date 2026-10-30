/*
 * SPDX-FileCopyrightText: 2020-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize serial bridge
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t serial_bridge_init(void);

/**
 * @brief Feed data coming from the target into the USB CDC IN endpoint.
 *
 * Called by the transport glue: from the local UART event task in wired mode and
 * from the ESP-NOW receive callback in wireless host mode.
 *
 * @param data Received data
 * @param len  Data length
 */
void serial_bridge_target_data_received(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
