/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Optional addressable (WS2812) RGB status LED.
 *
 * The feature is switched on with CONFIG_BRIDGE_LED_ENABLE; with it disabled there is
 * no LED pin at all and every function below is a stub. When enabled, the pin is chosen
 * with CONFIG_BRIDGE_WS2812_GPIO (-1 means none). The driver is deliberately NOT
 * implemented yet: led_rgb_init() only validates the pin and records the intent, and the
 * pin is left in its reset state - it is never configured, never driven and never pulsed.
 * Boards whose pinout does not match the defaults therefore stay untouched, while the
 * future status LED has one place to land.
 *
 * Contract for the eventual driver (call sites already rely on it):
 *   - led_rgb_init() may then configure the pin and is still called once from app_main;
 *   - led_rgb_is_available() reports whether the LED can actually be driven;
 *   - led_rgb_set()/led_rgb_off() are cheap, safe from any task and never block for
 *     longer than one frame (~30 us per WS2812);
 *   - nothing here may touch the pin before led_rgb_init() has succeeded.
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * @brief Validate CONFIG_BRIDGE_WS2812_GPIO without touching the pin.
 *
 * @return ESP_OK when the pin is unassigned (-1) or accepted.
 * @return ESP_ERR_INVALID_ARG when the number is outside the chip's GPIO range or
 *         already used by another bridge function (TX/RX/BOOT/RST, TCK/TMS).
 */
esp_err_t led_rgb_init(void);

/*
 * @brief Whether the reserved LED can actually be driven. False until a WS2812
 *        driver exists, so callers can skip colour decisions entirely.
 */
bool led_rgb_is_available(void);

/*
 * @brief Ask for a colour. Currently logs once and returns ESP_ERR_NOT_SUPPORTED.
 */
esp_err_t led_rgb_set(uint8_t red, uint8_t green, uint8_t blue);

/*
 * @brief Turn the LED off (all channels zero). Same state rules as led_rgb_set().
 */
esp_err_t led_rgb_off(void);

#ifdef __cplusplus
}
#endif
