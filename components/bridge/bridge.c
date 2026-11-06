/*
 * SPDX-FileCopyrightText: 2026 Dbb
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Role independent bridge logic: the status LED hooks, the DTR/RTS handling of
 * the roles that own the target pins, the serial uplink seen by the USB device
 * component and the public start-up sequence. The role files add what is
 * specific to wired, wireless host and wireless slave.
 */

#include <inttypes.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "bridge.h"
#include "bridge_priv.h"
#include "status_led.h"
#include "target_uart.h"

#if BRIDGE_LOCAL_TARGET
#include "debug_probe.h"
#endif
#if BRIDGE_HAS_USB
#include "usb_device.h"
#endif
#if BRIDGE_WIRELESS
#include "wireless_link.h"
#endif

static const char *TAG = "bridge";

/* The wireless roles tunnel CMSIS-DAP, which only exists in the SWD build. */
#if BRIDGE_WIRELESS && defined(CONFIG_DEBUG_PROBE_IFACE_JTAG)
#error "The wireless bridge roles require 'END-config -> Role configuration -> Debug interface -> CMSIS-DAP SWD' (CONFIG_DEBUG_PROBE_IFACE_SWD)."
#endif

/* ================================================================ DAP session */

#define BRIDGE_DAP_ID_CONNECT    0x02u
#define BRIDGE_DAP_ID_DISCONNECT 0x03u

void bridge_dap_track_session(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return;
    }

    if (data[0] == BRIDGE_DAP_ID_CONNECT) {
        status_led_notify_swd_session(true);
    } else if (data[0] == BRIDGE_DAP_ID_DISCONNECT) {
        status_led_notify_swd_session(false);
    }
}

/* ========================================================== local target side */

#if BRIDGE_LOCAL_TARGET

static esp_timer_handle_t s_state_change_timer;

static void bridge_state_change_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "BOOT = 1, RST = 1");
    target_uart_set_boot_reset_pins(true, true);
}

static esp_err_t bridge_init_state_change_timer(void)
{
    if (s_state_change_timer) {
        return ESP_OK;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = bridge_state_change_timer_cb,
        .name = "bridge_state_change",
    };
    return esp_timer_create(&timer_args, &s_state_change_timer);
}

/* Pure mapping, no I/O: the DTR/RTS to BOOT/RST transformation follows the auto reset
   circuitry of the ESP development boards, see the ESP USB Bridge schematics. */
static void bridge_line_state_map(bool dtr, bool rts, bool *boot, bool *rst)
{
    /* defaults for ((dtr && rts) || (!dtr && !rts)) */
    *rst = true;
    *boot = true;

    if (!dtr && rts) {
        *rst = false;
        *boot = true;
    } else if (dtr && !rts) {
        *rst = true;
        *boot = false;
    }
}

/* Postpone the BOOT=1/RST=1 assignment until no other state change happens in the
   meantime. This is a patch for esptool, which emits DTR=0 & RTS=1 followed by
   DTR=1 & RTS=0 with a temporary DTR=1 & RTS=1 in between. */
static esp_err_t bridge_line_state_defer_defaults(void)
{
    ESP_ERROR_CHECK(esp_timer_start_once(s_state_change_timer, 10 * 1000 /*us*/));
    return ESP_OK;
}

/* Orchestration: stop the pending deferral, map, drive the pins, then restore the
   default baudrate and hold TDI low while the target leaves reset. */
esp_err_t bridge_apply_line_state(bool dtr, bool rts)
{
    esp_timer_stop(s_state_change_timer);  /* may not be running, the return value is not interesting */

    if (dtr && rts) {
        return bridge_line_state_defer_defaults();
    }

    bool boot;
    bool rst;
    bridge_line_state_map(dtr, rts, &boot, &rst);

    ESP_LOGI(TAG, "DTR = %d, RTS = %d -> BOOT = %d, RST = %d", dtr, rts, boot, rst);

    target_uart_set_boot_reset_pins(boot, rst);

    if (!rst) {
        const uint32_t default_baud = 115200;
        esp_err_t err = target_uart_set_baudrate(default_baud);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot restore the default baudrate: %s", esp_err_to_name(err));
            return err;
        }
    }

    /* On ESP32, TDI is on GPIO12, which is also a strapping pin that selects the flash
       voltage. It must be held low while the target leaves reset. */
    if (boot) {
        bridge_probe_handle_esp32_tdi_bootstrapping(!rst);
    }

    return ESP_OK;
}

void bridge_on_uart_data(const uint8_t *data, size_t len)
{
    status_led_notify_activity(STATUS_LED_ACT_SERIAL);

#if BRIDGE_WIRELESS
    wlink_send(WLINK_CHANNEL_SERIAL, 0, 0, data, len);
#else
    usb_device_target_data_received(data, len);
#endif
}

esp_err_t bridge_local_target_start(void)
{
    esp_err_t err = bridge_init_state_change_timer();
    if (err != ESP_OK) {
        return err;
    }

    err = target_uart_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART transport init failed: %s", esp_err_to_name(err));
        return err;
    }

    return target_uart_register_rx_cb(bridge_on_uart_data);
}

#endif /* BRIDGE_LOCAL_TARGET */

/* ============================================================ wireless handlers */

#if BRIDGE_WIRELESS

/*
 * The status LED pulls the link state instead of being told about it, so nothing has to
 * hook into the ESP-NOW callbacks. wlink_link_up() is a passive observation with its own
 * timeout: wlink_peer_ready() must keep meaning "the MAC is known", because the SWD
 * stop-and-wait logic depends on that and would otherwise start flapping.
 */
const status_led_link_ops_t k_bridge_led_link_ops = {
    .link_up = wlink_link_up,
};

void bridge_on_link_serial(wlink_channel_t channel, uint8_t sub, uint8_t seq,
                           const uint8_t *payload, size_t len)
{
    (void)channel;
    (void)sub;
    (void)seq;
    status_led_notify_activity(STATUS_LED_ACT_SERIAL);

#if BRIDGE_ROLE_ID == BRIDGE_ROLE_HOST_ID
    usb_device_target_data_received(payload, len);
#else
    target_uart_send(payload, len);
#endif
}

#endif /* BRIDGE_WIRELESS */

/* ================================================================ USB uplink */

#if BRIDGE_HAS_USB

esp_err_t bridge_serial_send(const uint8_t *data, size_t len)
{
    status_led_notify_activity(STATUS_LED_ACT_SERIAL);

#if BRIDGE_ROLE_ID == BRIDGE_ROLE_HOST_ID
    if (!wlink_peer_ready()) {
        ESP_LOGD(TAG, "No peer yet, %u serial bytes dropped", (unsigned)len);
        return ESP_OK;  /* not an error: the peer may simply not be up yet */
    }
    return wlink_send(WLINK_CHANNEL_SERIAL, 0, 0, data, len);
#else
    return target_uart_send(data, len);
#endif
}

esp_err_t bridge_serial_set_baudrate(uint32_t baud)
{
#if BRIDGE_ROLE_ID == BRIDGE_ROLE_HOST_ID
    uint8_t payload[sizeof(uint32_t)];
    memcpy(payload, &baud, sizeof(baud));

    if (!wlink_peer_ready()) {
        ESP_LOGD(TAG, "No peer yet, baudrate %" PRIu32 " not forwarded", baud);
        return ESP_OK;
    }
    return wlink_send(WLINK_CHANNEL_SERIAL_CMD, BRIDGE_SERCMD_LINE_CODING, 0, payload, sizeof(payload));
#else
    return target_uart_set_baudrate(baud);
#endif
}

esp_err_t bridge_serial_set_line_state(bool dtr, bool rts)
{
#if BRIDGE_ROLE_ID == BRIDGE_ROLE_HOST_ID
    const uint8_t payload[2] = { dtr ? 1 : 0, rts ? 1 : 0 };

    if (!wlink_peer_ready()) {
        ESP_LOGD(TAG, "No peer yet, DTR/RTS not forwarded");
        return ESP_OK;
    }
    return wlink_send(WLINK_CHANNEL_SERIAL_CMD, BRIDGE_SERCMD_LINE_STATE, 0, payload, sizeof(payload));
#else
    return bridge_apply_line_state(dtr, rts);
#endif
}

/*
 * The USB device side knows nothing about the roles: it calls back into this table.
 * Wired and wireless host differ only in what serial_send/set_baudrate/set_line_state
 * do; the probe callbacks are the same entry points the vendor bulk path uses.
 */
static const usb_device_ops_t k_bridge_usb_ops = {
    .serial_send = bridge_serial_send,
    .serial_set_baudrate = bridge_serial_set_baudrate,
    .serial_set_line_state = bridge_serial_set_line_state,
    .probe_init = bridge_probe_init,
    .probe_process_data = bridge_probe_process_data,
    .probe_get_data_to_send = bridge_probe_get_data_to_send,
    .probe_free_sent_data = bridge_probe_free_sent_data,
    .probe_handle_command = bridge_probe_handle_command,
    .probe_get_proto_caps = bridge_probe_get_proto_caps,
    .probe_handle_esp32_tdi_bootstrapping = bridge_probe_handle_esp32_tdi_bootstrapping,
};

#endif /* BRIDGE_HAS_USB */

/* ================================================================== SWD engine */

#if BRIDGE_LOCAL_TARGET

/*
 * A wired bridge reaches its DAP engine through the USB vendor interface, so this
 * branch is where its SWD traffic and session edges show up. The wireless slave
 * already notifies from the link receive path; repeating it here is harmless and
 * keeps the wired role on the same code.
 */
esp_err_t bridge_probe_init(void)
{
    return debug_probe_init();
}

esp_err_t bridge_probe_process_data(const uint8_t *data, size_t len)
{
    status_led_notify_activity(STATUS_LED_ACT_SWD);
    bridge_dap_track_session(data, len);
    return debug_probe_process_data(data, len);
}

uint8_t *bridge_probe_get_data_to_send(size_t *len, TickType_t timeout)
{
    uint8_t *data = debug_probe_get_data_to_send(len, timeout);

    if (data != NULL) {
        status_led_notify_activity(STATUS_LED_ACT_SWD);
    }

    return data;
}

void bridge_probe_free_sent_data(uint8_t *data)
{
    debug_probe_free_sent_data(data);
}

esp_err_t bridge_probe_handle_command(uint8_t cmd, uint16_t wValue,
                                      const uint8_t **out_data, size_t *out_len)
{
    return debug_probe_handle_command(cmd, wValue, out_data, out_len);
}

int bridge_probe_get_proto_caps(void *dest)
{
    return debug_probe_get_proto_caps(dest);
}

void bridge_probe_handle_esp32_tdi_bootstrapping(bool rebooting)
{
    debug_probe_handle_esp32_tdi_bootstrapping(rebooting);
}

#endif /* BRIDGE_LOCAL_TARGET */

/* ============================================================ public entry point */

esp_err_t bridge_start(void)
{
    esp_err_t err = ESP_OK;

#if BRIDGE_HAS_USB
    err = usb_device_register_ops(&k_bridge_usb_ops);
    if (err != ESP_OK) {
        return err;
    }
#endif

#if BRIDGE_ROLE_ID == BRIDGE_ROLE_WIRED_ID
    err = bridge_wired_init();
    if (err == ESP_OK) {
        err = usb_device_start();
    }
#elif BRIDGE_ROLE_ID == BRIDGE_ROLE_HOST_ID
    err = bridge_host_init();
    if (err == ESP_OK) {
        err = usb_device_start();
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ESP USB Bridge running as wireless host (USB <-> ESP-NOW)");
    }
#else
    err = bridge_slave_init();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ESP USB Bridge running as wireless slave (UART + SWD over ESP-NOW)");
    }
#endif

    return err;
}
