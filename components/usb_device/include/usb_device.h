/*
 * SPDX-FileCopyrightText: 2026 Dbb
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/*
 * USB device side of the bridge.
 *
 * Everything that talks to the host lives in this component: the tinyusb stack, the
 * descriptors (CDC serial + vendor bulk for the debug probe), the MS OS 2.0
 * descriptor and the CDC/vendor class glue. It knows nothing about the bridge roles.
 *
 * The reverse direction (USB -> target) goes through the ops table below, which
 * components/bridge registers before usb_device_start() is called:
 *
 *   host -> CDC data        -> usb_device_ops_t::serial_send()
 *   host -> line coding     -> usb_device_ops_t::serial_set_baudrate()
 *   host -> DTR/RTS         -> usb_device_ops_t::serial_set_line_state()
 *   host -> vendor control  -> usb_device_ops_t::probe_handle_command()
 *   host -> vendor bulk OUT -> usb_device_ops_t::probe_process_data()
 *   vendor bulk IN          <- usb_device_ops_t::probe_get_data_to_send()
 *   vendor bulk IN, failure <- usb_device_vendor_abort_pending()
 *
 * Only USB-capable targets (ESP32-S2/S3) build this component. On the others
 * (C3/C6, wireless slave only) the component is present but contributes no
 * sources; nothing in those images refers to usb_device_start().
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callbacks implemented by the bridge for the USB device side.
 *
 * All pointers must be valid for the lifetime of the image; the USB stack calls
 * them from the tinyusb task or from the CDC/vendor callbacks.
 */
typedef struct usb_device_ops {
    /**
     * @brief Send bytes received from the host towards the target.
     *
     * Wired mode: straight to the target UART. Wireless host mode: tunnelled to
     * the slave over ESP-NOW.
     */
    esp_err_t (*serial_send)(const uint8_t *data, size_t len);

    /**
     * @brief Apply (or forward) a line coding change.
     */
    esp_err_t (*serial_set_baudrate)(uint32_t baud);

    /**
     * @brief Apply (or forward) a DTR/RTS change.
     */
    esp_err_t (*serial_set_line_state)(bool dtr, bool rts);

    /**
     * @brief Initialise the debug probe path once the host is attached.
     */
    esp_err_t (*probe_init)(void);

    /**
     * @brief Handle a DAP packet received on the vendor bulk OUT endpoint.
     */
    esp_err_t (*probe_process_data)(const uint8_t *data, size_t len);

    /**
     * @brief Get the next packet to send on the vendor bulk IN endpoint.
     *
     * @param[out] len     Packet length.
     * @param[in]  timeout How long to wait for a packet.
     * @return Buffer owned by the bridge, freed with probe_free_sent_data().
     */
    uint8_t *(*probe_get_data_to_send)(size_t *len, TickType_t timeout);

    /**
     * @brief Return a buffer obtained from probe_get_data_to_send().
     */
    void (*probe_free_sent_data)(uint8_t *data);

    /**
     * @brief Handle a vendor control request.
     *
     * @param[out] out_data Response payload owned by the bridge (may be NULL).
     * @param[out] out_len  Response payload length.
     */
    esp_err_t (*probe_handle_command)(uint8_t cmd, uint16_t wValue,
                                      const uint8_t **out_data, size_t *out_len);

    /**
     * @brief Fill the probe capability string for its vendor string descriptor.
     *
     * @param dest Buffer of USB_DEVICE_MAX_STR_DESCRIPTOR_LEN bytes.
     * @return Number of bytes written (even, the descriptor is UTF-16).
     */
    int (*probe_get_proto_caps)(void *dest);

    /**
     * @brief Tell the probe backend that the target is leaving reset.
     */
    void (*probe_handle_esp32_tdi_bootstrapping)(bool rebooting);
} usb_device_ops_t;

/**
 * @brief Register the bridge callbacks.
 *
 * Must be called before usb_device_start().
 *
 * @param ops Callback table (must stay valid; normally a static const object).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for a NULL table.
 */
esp_err_t usb_device_register_ops(const usb_device_ops_t *ops);

/**
 * @brief Bring up the USB device stack.
 *
 * Fills the serial-number descriptor from the eFuse MAC, initialises the OTG PHY,
 * creates the CDC ring buffer and starts the tinyusb task. The probe backend is
 * initialised by the mount callback, through usb_device_ops_t::probe_init().
 *
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t usb_device_start(void);

/**
 * @brief Fail the vendor bulk IN transfer that is waiting for an answer.
 *
 * CMSIS-DAP has no response timeout: once the host starts a bulk IN read it only
 * completes when the device sends data. So when the probe backend decides that no
 * answer will come in time (the wireless relay gave up on a DAP packet), the IN
 * endpoint is stalled instead, which makes the host see a transfer error and lets
 * it retry. The stall is cleared again when the next OUT packet arrives.
 *
 * Safe to call when the probe backend already delivered data: the call is then a
 * no-op, because no answer is outstanding.
 */
void usb_device_vendor_abort_pending(void);

/**
 * @brief Feed bytes coming from the target into the USB CDC IN endpoint.
 *
 * Called by the bridge transport (local target UART in wired mode, ESP-NOW in
 * wireless host mode).
 *
 * @param data Received data
 * @param len  Data length
 */
void usb_device_target_data_received(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
