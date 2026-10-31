/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "led_rgb.h"

#include <stdbool.h>
#include <stddef.h>

#include "esp_log.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

static const char *TAG = "led_rgb";

/* The reserved pin is recorded but never claimed: no gpio_config(), no
   gpio_set_level(), no gpio_reset_pin(). Keeping it untouched is the whole point
   of this module until the RMT based driver is finished. */
static int s_pin = -1;
static bool s_rejected;

#if CONFIG_BRIDGE_WS2812_GPIO >= 0
/* Pins the rest of the firmware drives. A collision would make a status LED fight
   a functional signal (the C3 in-package flash pins are exactly this failure), so a
   hit is reported as a configuration error instead of being driven anyway. */
static const struct {
    const char *name;
    int pin;
} s_used_pins[] = {
    { "CONFIG_SERIAL_HANDLER_GPIO_TXD", CONFIG_SERIAL_HANDLER_GPIO_TXD },
    { "CONFIG_SERIAL_HANDLER_GPIO_RXD", CONFIG_SERIAL_HANDLER_GPIO_RXD },
    { "CONFIG_SERIAL_HANDLER_GPIO_BOOT", CONFIG_SERIAL_HANDLER_GPIO_BOOT },
    { "CONFIG_SERIAL_HANDLER_GPIO_RST", CONFIG_SERIAL_HANDLER_GPIO_RST },
    { "CONFIG_DEBUG_PROBE_GPIO_TCK", CONFIG_DEBUG_PROBE_GPIO_TCK },
    { "CONFIG_DEBUG_PROBE_GPIO_TMS", CONFIG_DEBUG_PROBE_GPIO_TMS },
#if CONFIG_DEBUG_PROBE_IFACE_JTAG
    { "CONFIG_DEBUG_PROBE_GPIO_TDI", CONFIG_DEBUG_PROBE_GPIO_TDI },
    { "CONFIG_DEBUG_PROBE_GPIO_TDO", CONFIG_DEBUG_PROBE_GPIO_TDO },
#endif
#if CONFIG_BRIDGE_LED_ENABLE
    { "CONFIG_BRIDGE_GPIO_LED1", CONFIG_BRIDGE_GPIO_LED1 },
    { "CONFIG_BRIDGE_GPIO_LED2", CONFIG_BRIDGE_GPIO_LED2 },
    { "CONFIG_BRIDGE_GPIO_LED3", CONFIG_BRIDGE_GPIO_LED3 },
#endif
};
#endif /* CONFIG_BRIDGE_WS2812_GPIO >= 0 */

esp_err_t led_rgb_init(void)
{
    const int pin = CONFIG_BRIDGE_WS2812_GPIO;

    if (pin < 0) {
        ESP_LOGI(TAG, "No WS2812 status LED reserved (CONFIG_BRIDGE_WS2812_GPIO=-1)");
        return ESP_OK;
    }

    if (pin >= SOC_GPIO_PIN_COUNT) {
        ESP_LOGE(TAG, "CONFIG_BRIDGE_WS2812_GPIO=%d is outside the GPIO range of this chip (0..%d)",
                 pin, SOC_GPIO_PIN_COUNT - 1);
        s_rejected = true;
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_BRIDGE_WS2812_GPIO >= 0
    for (size_t i = 0; i < sizeof(s_used_pins) / sizeof(s_used_pins[0]); ++i) {
        if (s_used_pins[i].pin == pin) {
            ESP_LOGE(TAG, "CONFIG_BRIDGE_WS2812_GPIO=%d collides with %s",
                     pin, s_used_pins[i].name);
            s_rejected = true;
            return ESP_ERR_INVALID_ARG;
        }
    }
#endif

    s_pin = pin;

    /* WARN, not INFO: the reservation is not a working LED yet, and the message is
       the reminder to finish the driver before relying on it. */
    ESP_LOGW(TAG, "GPIO%d reserved for a WS2812 status LED; no driver is implemented yet, "
                  "the pin stays in its reset state and is never driven", pin);
    return ESP_OK;
}

bool led_rgb_is_available(void)
{
    /* Flip this to "return s_pin >= 0" once the RMT driver is in place. */
    return false;
}

esp_err_t led_rgb_set(uint8_t red, uint8_t green, uint8_t blue)
{
    (void)red;
    (void)green;
    (void)blue;

    if (s_pin < 0 || s_rejected) {
        return ESP_ERR_INVALID_STATE;
    }

    static bool warned;
    if (!warned) {
        warned = true;
        ESP_LOGW(TAG, "WS2812 driver is not implemented; colour requests for GPIO%d are ignored",
                 s_pin);
    }

    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t led_rgb_off(void)
{
    return led_rgb_set(0, 0, 0);
}
