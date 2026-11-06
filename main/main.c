/*
 * SPDX-FileCopyrightText: 2020-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Application assembly only.
 *
 * The bridge component owns the role selection, the target UART, the ESP-NOW
 * link and the USB callback table; the USB device stack lives in
 * components/usb_device. main.c starts the optional status LED, then hands over
 * to bridge_start() and reports what is left of the heap. It owns no descriptor,
 * no task and no transport detail.
 */

#include "esp_log.h"
#include "esp_system.h"

#include "bridge.h"
#include "status_led.h"

static const char *TAG = "bridge_main";

void app_main(void)
{
    /* The optional status LED. With CONFIG_BRIDGE_LED_ENABLE=n this is a stub that only
       logs, so no pin is ever configured. When enabled it validates the reserved WS2812
       pin, claims it and starts the rendering task; the link state source is registered
       later, by bridge_start(), once the ESP-NOW link exists. A rejected pin (range
       or collision) must not keep the bridge from running. */
    if (status_led_init() != ESP_OK) {
        ESP_LOGW(TAG, "Status LED configuration rejected, continuing without it");
    }

    /* bridge_start() picks the transport for the compiled role (the target UART in
       wired/slave mode, the ESP-NOW link in the wireless modes), registers the USB
       callback table when the build has a USB side and starts the device. */
    ESP_ERROR_CHECK(bridge_start());

    /* Startup is over: report how much heap is left. The single-core C3 has 400 KB of
       SRAM shared with the Wi-Fi stack, so this line is the cheapest way to notice that
       a new buffer or stack has eaten the margin. */
    ESP_LOGI(TAG, "Free heap after start-up: %u bytes", (unsigned)esp_get_free_heap_size());
}
