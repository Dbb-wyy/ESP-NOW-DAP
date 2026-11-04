/*
 * SPDX-FileCopyrightText: 2020-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SERIAL_HANDLER_TASK_PRI 5

#define DOWNLOAD_MODE  1
#define RUNNING_MODE   0
#define RESET_ASSERT   1
#define RESET_RELEASE  0

typedef enum {
    TRANSPORT_TYPE_UART,
    // Future transport types...
} transport_type_t;

/**
 * @brief Data received callback - called when data is received from transport
 *
 * NOTE: This callback is only invoked when flashing is NOT in progress.
 * When flashing is active, all received data is used internally for ESP loader operations.
 *
 * @param data Received data buffer
 * @param len Length of received data
 */
typedef void (*transport_data_received_cb_t)(const uint8_t *data, size_t len);

/**
 * @brief Register callback for received data
 *
 * The callback will be invoked when data is received, but ONLY when
 * flashing is not in progress. During flashing operations, all data
 * is handled internally by the ESP loader.
 *
 * @param callback Callback function to call when data is received
 * @return esp_err_t ESP_OK on success
 */
esp_err_t serial_handler_register_data_callback(transport_data_received_cb_t callback);

/**
 * @brief Initialize comm handler with specified transport type
 *
 * @param type Transport type to initialize (UART, SPI, I2C, etc.)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t serial_handler_init(transport_type_t type);

/**
 * @brief Send data through transport
 *
 * Can be used for bridge operations when flashing is not active,
 * or for ESP loader operations when flashing is active.
 *
 * @param data Data to send
 * @param len Length of data
 * @return esp_err_t ESP_OK on success
 */
esp_err_t serial_handler_send_data(const uint8_t *data, size_t len);

/**
 * @brief Set transport baudrate/speed
 *
 * @param baud Baudrate for UART, clock speed for SPI, etc.
 * @return esp_err_t ESP_OK on success
 */
esp_err_t serial_handler_set_baudrate(uint32_t baud);

/**
 * @brief Set target boot and reset pin levels
 *
 * This function sets the GPIO levels of both BOOT and RESET pins.
 * The function handles the pin state internally based on the provided values.
 *
 * @param boot_pin true to set boot pin HIGH, false to set it LOW
 * @param reset_pin true to set reset pin HIGH, false to set it LOW
 */
void serial_handler_set_boot_reset_pins(bool boot_pin, bool reset_pin);

#ifdef __cplusplus
}
#endif
