/*
 * SPDX-FileCopyrightText: 2020-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Data received callback - called when data arrives from the target UART.
 *
 * @param data Received data buffer
 * @param len Length of received data
 */
typedef void (*target_uart_rx_cb_t)(const uint8_t *data, size_t len);

/**
 * @brief Register the callback for data received from the target UART.
 *
 * @param callback Callback function to call when data is received
 * @return esp_err_t ESP_OK on success
 */
esp_err_t target_uart_register_rx_cb(target_uart_rx_cb_t callback);

/**
 * @brief Initialize the target UART and the BOOT/RESET lines.
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t target_uart_init(void);

/**
 * @brief Send data to the target UART.
 *
 * @param data Data to send
 * @param len Length of data
 * @return esp_err_t ESP_OK on success
 */
esp_err_t target_uart_send(const uint8_t *data, size_t len);

/**
 * @brief Set the target UART baudrate.
 *
 * @param baud New baudrate
 * @return esp_err_t ESP_OK on success
 */
esp_err_t target_uart_set_baudrate(uint32_t baud);

/**
 * @brief Set target boot and reset pin levels
 *
 * This function sets the GPIO levels of both BOOT and RESET pins.
 * The function handles the pin state internally based on the provided values.
 *
 * @param boot_pin true to set boot pin HIGH, false to set it LOW
 * @param reset_pin true to set reset pin HIGH, false to set it LOW
 */
void target_uart_set_boot_reset_pins(bool boot_pin, bool reset_pin);

#ifdef __cplusplus
}
#endif