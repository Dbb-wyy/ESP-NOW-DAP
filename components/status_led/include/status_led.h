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
 * Addressable (WS2812) status LED.
 *
 * The feature is switched on with CONFIG_BRIDGE_LED_ENABLE; with it disabled (which is
 * always the case for the wired role, see the Kconfig dependency) there is no LED pin at
 * all and the implementation in status_led.c compiles to no-op functions that ignore their
 * arguments. A call site therefore never needs an #if.
 *
 * The LED reports the state of the bridge, not the state of the USB device: while the
 * ESP-NOW link is up it breathes cyan, while the link is down it flashes amber, and
 * recent SWD or serial traffic takes over with blue, green or violet.
 *
 * The blue "SWD in use" state is a session, not traffic: it is opened by the CMSIS-DAP
 * Connect command and closed by Disconnect, so a debugger sitting on a halted target -
 * which sends nothing at all for as long as the user thinks - keeps its colour. Serial
 * activity stays packet based, there is no session to latch on to.
 *
 * The renderer is a small state machine with one way in: the status is sampled every
 * half activity window, the sample is handed to a single dispatch function, and each
 * state has its own one line function that only picks a colour and a movement. Drawing
 * runs on a faster tick, so the sampling period shapes when the state changes but never
 * the animation. One state per line, one place to recolour it.
 *
 * Activity is pushed - a call site only stores a timestamp, which is a single 32 bit
 * write and therefore cheap enough for the USB and Wi-Fi callbacks - while the link
 * state is pulled through status_led_link_ops_t, so this module stays independent of the
 * wireless link implementation.
 *
 * Contract:
 *   - status_led_init() is called once from app_main; it validates the pin and, when
 *     accepted, starts the rendering task. A rejected pin is reported, never fatal.
 *   - status_led_notify_activity() is safe from any task and never blocks.
 *   - the colour is always chosen by the state machine below; there is no API to force
 *     one, so nothing can fight it.
 *   - nothing here touches the pin before status_led_init() has succeeded.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Events that temporarily take over the LED. Order does not matter, the renderer picks. */
typedef enum {
    STATUS_LED_ACT_SWD = 0,    /*!< a CMSIS-DAP packet was relayed in either direction */
    STATUS_LED_ACT_SERIAL,     /*!< serial bytes were relayed in either direction */
    STATUS_LED_ACT_MAX,
} status_led_activity_t;

/**
 * Link state source. The callback is invoked from the LED task and must not block; it is
 * expected to be cheap, it runs once per state poll, that is every
 * CONFIG_BRIDGE_LED_ACT_WINDOW_MS / 2 milliseconds.
 */
typedef struct {
    bool (*link_up)(void);  /*!< true while the peer is reachable */
} status_led_link_ops_t;

/**
 * @brief Validate CONFIG_BRIDGE_WS2812_GPIO, claim the pin and start rendering.
 *
 * @return ESP_OK when the pin is unassigned (-1) or accepted and the task is running.
 * @return ESP_ERR_INVALID_ARG when the number is outside the chip's GPIO range or
 *         already used by another bridge function (TX/RX/BOOT/RST, TCK/TMS).
 * @return ESP_ERR_NO_MEM / the RMT error when the strip could not be created.
 */
esp_err_t status_led_init(void);

/**
 * @brief Register the link state source. The pointer must outlive the caller's use of
 *        the LED; every current caller passes a static const structure.
 */
void status_led_set_link_ops(const status_led_link_ops_t *ops);

/**
 * @brief Stamp one activity event. Cheap, lock free and safe from any task, including
 *        the USB and Wi-Fi callbacks.
 */
void status_led_notify_activity(status_led_activity_t act);

/**
 * @brief Open or close the SWD session that keeps the blue state latched.
 *
 * Call with true on a CMSIS-DAP Connect command and false on Disconnect. Opening the
 * session is cheap and lock free like the activity helpers. CONFIG_BRIDGE_LED_SESSION_TIMEOUT_MS
 * defaults to 0, which means only Disconnect closes the session; a non-zero value is a safety
 * net for a debugger that dies without sending one, refreshed by packets inside the session.
 */
void status_led_notify_swd_session(bool active);

#ifdef __cplusplus
}
#endif
