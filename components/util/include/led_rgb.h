/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

/*
 * Addressable (WS2812) status LED.
 *
 * The feature is switched on with CONFIG_BRIDGE_LED_ENABLE; with it disabled (which is
 * always the case for the wired role, see the Kconfig dependency) there is no LED pin at
 * all and every function below either compiles to a stub or, for the two notifying
 * helpers, to an empty inline function. A call site therefore never needs an #if.
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
 * state is pulled through led_rgb_link_ops_t, so this module stays independent of the
 * wireless link implementation.
 *
 * Contract:
 *   - led_rgb_init() is called once from app_main; it validates the pin and, when
 *     accepted, starts the rendering task. A rejected pin is reported, never fatal.
 *   - led_rgb_is_available() reports whether the LED can actually be driven.
 *   - led_rgb_notify_activity() is safe from any task and never blocks.
 *   - led_rgb_set()/led_rgb_off() override the state machine for a short while and are
 *     the only calls that may block, for one WS2812 frame (~30 us).
 *   - nothing here touches the pin before led_rgb_init() has succeeded.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Events that temporarily take over the LED. Order does not matter, the renderer picks. */
typedef enum {
    LED_RGB_ACT_SWD = 0,    /*!< a CMSIS-DAP packet was relayed in either direction */
    LED_RGB_ACT_SERIAL,     /*!< serial bytes were relayed in either direction */
    LED_RGB_ACT_MAX,
} led_rgb_activity_t;

/**
 * Link state source. Both callbacks are invoked from the LED task and must not block;
 * @p link_up is expected to be cheap, it runs once per state poll, that is every
 * CONFIG_BRIDGE_LED_ACT_WINDOW_MS / 2 milliseconds.
 */
typedef struct {
    bool (*link_up)(void);              /*!< true while the peer is reachable */
    uint32_t (*peer_last_seen_ms)(void);/*!< milliseconds since the last frame, 0 = never */
} led_rgb_link_ops_t;

/**
 * @brief Validate CONFIG_BRIDGE_WS2812_GPIO, claim the pin and start rendering.
 *
 * @return ESP_OK when the pin is unassigned (-1) or accepted and the task is running.
 * @return ESP_ERR_INVALID_ARG when the number is outside the chip's GPIO range or
 *         already used by another bridge function (TX/RX/BOOT/RST, TCK/TMS).
 * @return ESP_ERR_NO_MEM / the RMT error when the strip could not be created.
 */
esp_err_t led_rgb_init(void);

/** @brief Whether the LED is actually being driven. */
bool led_rgb_is_available(void);

/**
 * @brief Register the link state source. The pointer must outlive the caller's use of
 *        the LED; every current caller passes a static const structure.
 */
#if CONFIG_BRIDGE_LED_ENABLE
void led_rgb_set_link_ops(const led_rgb_link_ops_t *ops);
#else
static inline void led_rgb_set_link_ops(const led_rgb_link_ops_t *ops) { (void)ops; }
#endif

/**
 * @brief Stamp one activity event. Cheap, lock free and safe from any task, including
 *        the USB and Wi-Fi callbacks.
 */
#if CONFIG_BRIDGE_LED_ENABLE
void led_rgb_notify_activity(led_rgb_activity_t act);
#else
static inline void led_rgb_notify_activity(led_rgb_activity_t act) { (void)act; }
#endif

/**
 * @brief Open or close the SWD session that keeps the blue state latched.
 *
 * Call with true on a CMSIS-DAP Connect command and false on Disconnect. Opening the
 * session is cheap and lock free like the activity helpers. CONFIG_BRIDGE_LED_SESSION_TIMEOUT_MS
 * defaults to 0, which means only Disconnect closes the session; a non-zero value is a safety
 * net for a debugger that dies without sending one, refreshed by packets inside the session.
 */
#if CONFIG_BRIDGE_LED_ENABLE
void led_rgb_notify_swd_session(bool active);
#else
static inline void led_rgb_notify_swd_session(bool active) { (void)active; }
#endif

/**
 * @brief Show one colour for about a second, then resume the state machine.
 *
 * @return ESP_OK, or ESP_ERR_INVALID_STATE when the LED is not available.
 */
esp_err_t led_rgb_set(uint8_t red, uint8_t green, uint8_t blue);

/**
 * @brief Turn the LED off immediately and hand control back to the state machine.
 */
esp_err_t led_rgb_off(void);

#ifdef __cplusplus
}
#endif
