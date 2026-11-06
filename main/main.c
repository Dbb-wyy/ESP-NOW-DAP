/*
 * SPDX-FileCopyrightText: 2020-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Application assembly only.
 *
 * The USB device stack lives in components/usb_device and the role glue in
 * bridge_glue.c. main.c picks the pieces for the compiled role, wires the USB
 * callbacks to the bridge (bridge_init() registers them) and starts the device
 * side. It owns no descriptor, no task beyond the boot sequence and no transport
 * detail.
 */

#include <stdlib.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_system.h"

#include "bridge_glue.h"
#include "status_led.h"

#if !CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE
#include "usb_device.h"
#endif

static const char *TAG = "bridge_main";

void app_main(void)
{
    /* The optional status LED. With CONFIG_BRIDGE_LED_ENABLE=n this is a stub that only
       logs, so no pin is ever configured. When enabled it validates the reserved WS2812
       pin, claims it and starts the rendering task; the link state source is registered
       later, by bridge_init(), once the ESP-NOW link exists. A rejected pin (range
       or collision) must not keep the bridge from running. */
    if (status_led_init() != ESP_OK) {
        ESP_LOGW(TAG, "Status LED configuration rejected, continuing without it");
    }

    /* bridge_init() picks the transport for the compiled role (local target UART in
       wired/slave mode, the ESP-NOW link in the wireless modes) and registers the
       USB callback table when the build has a USB side. */
    ESP_ERROR_CHECK(bridge_init());

#if CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE
    /* Target side: no USB at all, the ESP-NOW link is the only host interface. */
    ESP_ERROR_CHECK(bridge_wireless_slave_start());
    ESP_LOGI(TAG, "ESP USB Bridge running as wireless slave (UART + SWD over ESP-NOW)");
#else
    ESP_ERROR_CHECK(usb_device_start());
  #if CONFIG_BRIDGE_ROLE_WIRELESS_HOST
    ESP_LOGI(TAG, "ESP USB Bridge running as wireless host (USB <-> ESP-NOW)");
  #endif
#endif

    /* Startup is over: report how much heap is left. The single-core C3 has 400 KB of
       SRAM shared with the Wi-Fi stack, so this line is the cheapest way to notice that
       a new buffer or stack has eaten the margin. */
    ESP_LOGI(TAG, "Free heap after start-up: %u bytes", (unsigned)esp_get_free_heap_size());
}
