/*
 * SPDX-FileCopyrightText: 2026 Dbb
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Private to components/bridge: the compiled role, its capabilities and the
 * entry points the role files share.
 *
 * The role is a compile-time choice, but only this header reads the Kconfig
 * symbols; the .c files work with the numeric BRIDGE_ROLE_* identifiers so the
 * role knowledge stays in one place.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#include "status_led.h"

/* Values of BRIDGE_ROLE_ID; only the preprocessor uses them. */
#define BRIDGE_ROLE_WIRED_ID 0
#define BRIDGE_ROLE_HOST_ID  1
#define BRIDGE_ROLE_SLAVE_ID 2

#if CONFIG_BRIDGE_ROLE_WIRED
#define BRIDGE_ROLE_ID BRIDGE_ROLE_WIRED_ID
#elif CONFIG_BRIDGE_ROLE_WIRELESS_HOST
#define BRIDGE_ROLE_ID BRIDGE_ROLE_HOST_ID
#else
#define BRIDGE_ROLE_ID BRIDGE_ROLE_SLAVE_ID
#endif

/* A build with a USB stack: wired and wireless host. */
#define BRIDGE_HAS_USB (BRIDGE_ROLE_ID != BRIDGE_ROLE_SLAVE_ID)

/* The roles that own the target pins and the SWD engine directly. */
#define BRIDGE_LOCAL_TARGET (BRIDGE_ROLE_ID != BRIDGE_ROLE_HOST_ID)

/* The roles that talk over ESP-NOW. */
#define BRIDGE_WIRELESS (BRIDGE_ROLE_ID != BRIDGE_ROLE_WIRED_ID)

/* Maximum DAP packet on the wire, matches DAP_PACKET_SIZE / EUB_VENDORD_EPSIZE. */
#define BRIDGE_SWD_PKT_SIZE 64

/*
 * Depth of the slave SWD request queue, and of the sequence list that tracks the
 * requests which were accepted but not answered yet. The list has to cover the
 * queue plus whatever the DAP engine still holds (1024 byte ring buffers holding
 * 64 byte packets); the host is stop-and-wait, so in practice one or two entries
 * are in use.
 */
#define BRIDGE_SWD_REQ_QUEUE_DEPTH 8
#define BRIDGE_SWD_SEQ_FIFO_DEPTH 32

/* Sub types carried on WLINK_CHANNEL_SERIAL_CMD (the link frame envelope is private). */
#define BRIDGE_SERCMD_LINE_CODING 0u   /*!< payload: uint32_t baud, little endian */
#define BRIDGE_SERCMD_LINE_STATE  1u   /*!< payload: {dtr, rts} */

/* How often the host relay re-checks the link while it waits for the peer. */
#define BRIDGE_PEER_POLL_MS 100

/* Same value as the debug_probe worker task priority. */
#define BRIDGE_TASK_PRI 5

/*
 * Per packet logging on the SWD path. It stays at debug level by default,
 * because a debug session can send hundreds of DAP commands per second and the
 * serial console is far slower than that; CONFIG_WLINK_SWD_VERBOSE_LOG promotes
 * it to info level when the path has to be diagnosed. State changes and
 * anomalies are logged unconditionally.
 */
#if CONFIG_WLINK_SWD_VERBOSE_LOG
#define BRIDGE_SWD_PKT_LOG(fmt, ...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#else
#define BRIDGE_SWD_PKT_LOG(fmt, ...) ESP_LOGD(TAG, fmt, ##__VA_ARGS__)
#endif

#if BRIDGE_WIRELESS
#include "wireless_link.h"
#endif

#if BRIDGE_HAS_USB
#include "usb_device.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Every role file defines its entry point; bridge_start() calls the compiled one. */
esp_err_t bridge_wired_init(void);
esp_err_t bridge_host_init(void);
esp_err_t bridge_slave_init(void);

/*
 * The CMSIS-DAP bulk backend carries no report ID, so the command is the first
 * payload byte. Watching it gives the status LED a session edge to latch on
 * instead of raw traffic: a debugger sitting on a halted target sends nothing at
 * all for minutes and the blue state still has to stay on.
 */
void bridge_dap_track_session(const uint8_t *data, size_t len);

#if BRIDGE_WIRELESS
/* ESP-NOW receive callback for the serial channel (host forwards to USB, slave to UART). */
void bridge_on_link_serial(wlink_channel_t channel, uint8_t sub, uint8_t seq,
                           const uint8_t *payload, size_t len);

/* Pulled by the status LED: a passive look at whether the link is up right now. */
extern const status_led_link_ops_t k_bridge_led_link_ops;
#endif

#if BRIDGE_LOCAL_TARGET
/* Target UART, BOOT/RST timer and the UART receive callback, shared by wired and slave. */
esp_err_t bridge_local_target_start(void);

/* DTR/RTS to BOOT/RST: map, drive the pins, restore the default baudrate. */
esp_err_t bridge_apply_line_state(bool dtr, bool rts);

/* Target UART data: report the activity, then hand it to the USB side or the link. */
void bridge_on_uart_data(const uint8_t *data, size_t len);
#endif

/*
 * Debug probe entry points. All three roles provide them: wired and slave call the
 * local engine, the wireless host relays over the link.
 */
esp_err_t bridge_probe_init(void);
esp_err_t bridge_probe_process_data(const uint8_t *data, size_t len);
uint8_t *bridge_probe_get_data_to_send(size_t *len, TickType_t timeout);
void bridge_probe_free_sent_data(uint8_t *data);
esp_err_t bridge_probe_handle_command(uint8_t cmd, uint16_t wValue,
                                      const uint8_t **out_data, size_t *out_len);
int bridge_probe_get_proto_caps(void *dest);
void bridge_probe_handle_esp32_tdi_bootstrapping(bool rebooting);

#if BRIDGE_HAS_USB
/* The serial uplink as seen by the USB device component. */
esp_err_t bridge_serial_send(const uint8_t *data, size_t len);
esp_err_t bridge_serial_set_baudrate(uint32_t baud);
esp_err_t bridge_serial_set_line_state(bool dtr, bool rts);
#endif

#ifdef __cplusplus
}
#endif
