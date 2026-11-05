/*
 * SPDX-FileCopyrightText: 2026 Dbb
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/*
 * Role dispatching glue.
 *
 * The bridge always has two ends:
 *   HOST  end : USB (CDC serial + vendor bulk for the debug probe)
 *   SLAVE end : UART + SWD pins of the target
 *
 * In wired mode both ends live in the same chip. In the wireless modes they are
 * split across two chips and connected by the ESP-NOW link, so every call that
 * used to go straight to the local transport now goes through this header.
 *
 *   role          serial uplink        debug probe
 *   ------------------------------------------------------------------
 *   WIRED         local UART           local debug_probe (JTAG or CMSIS-DAP)
 *   WIRELESS_HOST USB  -> ESP-NOW      USB bulk -> ESP-NOW relay
 *   WIRELESS_SLAVE ESP-NOW -> UART     ESP-NOW -> local CMSIS-DAP engine
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "debug_probe.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ serial uplink */

/**
 * @brief Initialise the serial uplink for the current role.
 *
 * WIRED/SLAVE: install the target UART through the serial handler.
 * WIRELESS_*: initialise the ESP-NOW link and register the serial handlers.
 */
esp_err_t bridge_uplink_init(void);

/**
 * @brief Send data towards the target (called from the USB CDC receive path).
 */
esp_err_t bridge_serial_send(const uint8_t *data, size_t len);

/**
 * @brief Apply / forward a line coding change (baud rate).
 */
esp_err_t bridge_serial_set_baudrate(uint32_t baud);

/**
 * @brief Apply / forward a DTR & RTS change.
 *
 * In wired and slave mode the DTR/RTS to BOOT/RST mapping (including the esptool
 * timing patch) is performed locally. In wireless host mode the raw levels are
 * tunnelled to the slave, which owns the BOOT/RST pins.
 */
esp_err_t bridge_serial_set_line_state(bool dtr, bool rts);

/**
 * @brief Start the wireless slave side: ESP-NOW link, target UART, local SWD engine.
 *
 * Must only be called when CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE is selected.
 */
esp_err_t bridge_wireless_slave_start(void);

/* ------------------------------------------------------------------ debug probe */

/**
 * @brief Initialise the debug probe path for the current role.
 *
 * WIRED/SLAVE: bring up the local debug_probe (JTAG or CMSIS-DAP).
 * WIRELESS_HOST: allocate the buffers and the relay task that tunnel DAP packets.
 */
esp_err_t bridge_probe_init(void);

esp_err_t bridge_probe_process_data(const uint8_t *data, size_t len);
uint8_t *bridge_probe_get_data_to_send(size_t *len, TickType_t timeout);
void bridge_probe_free_sent_data(uint8_t *data);
/**
 * @brief Handle a vendor control command through the probe backend.
 *
 * WIRED/SLAVE: forwarded to the local debug_probe backend.
 * WIRELESS_HOST: always ESP_ERR_NOT_SUPPORTED (the backend lives on the slave).
 *
 * @param[out] out_data Response payload owned by the backend (may be NULL).
 * @param[out] out_len  Response payload length.
 */
esp_err_t bridge_probe_handle_command(uint8_t cmd, uint16_t wValue,
                                      const uint8_t **out_data, size_t *out_len);
int bridge_probe_get_proto_caps(void *dest);
void bridge_probe_handle_esp32_tdi_bootstrapping(bool rebooting);

#ifdef __cplusplus
}
#endif
