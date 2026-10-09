/*
 * SPDX-FileCopyrightText: 2026 Dbb
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <inttypes.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"

#include "bridge_glue.h"
#include "serial_bridge.h"
#include "serial_handler.h"
#include "led_io.h"

#if !CONFIG_BRIDGE_ROLE_WIRED
#include "wireless_link.h"
#endif

static const char *TAG = "bridge_glue";

/* Roles that own the target pins directly. */
#define BRIDGE_LOCAL_TARGET (!CONFIG_BRIDGE_ROLE_WIRELESS_HOST)

/* The wireless roles tunnel CMSIS-DAP, which only exists in the SWD build. */
#if !CONFIG_BRIDGE_ROLE_WIRED && defined(CONFIG_DEBUG_PROBE_IFACE_JTAG)
#error "The wireless bridge roles require 'Debug interface -> CMSIS-DAP SWD' (CONFIG_DEBUG_PROBE_IFACE_SWD)."
#endif

/* Maximum DAP packet on the wire, matches DAP_PACKET_SIZE / EUB_VENDORD_EPSIZE. */
#define BRIDGE_SWD_PKT_SIZE 64

/* =================================================================== status LEDs */

void bridge_led_serial_tx(bool active)
{
    /* TX and RX LEDs are swapped so the LEDs show traffic in the bridge -> target direction. */
    gpio_set_level(LED_RX, active ? LED_RX_ON : LED_RX_OFF);
}

void bridge_led_serial_rx(bool active)
{
    gpio_set_level(LED_TX, active ? LED_TX_ON : LED_TX_OFF);
}

void bridge_led_debug(bool active)
{
    gpio_set_level(LED_JTAG, active ? LED_JTAG_ON : LED_JTAG_OFF);
}

/* ==================================================== local target side (wired/slave) */

#if BRIDGE_LOCAL_TARGET

static esp_timer_handle_t s_state_change_timer;
static bool s_tdi_bootstrapping;

static void bridge_state_change_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "BOOT = 1, RST = 1");
    serial_handler_set_boot_reset_pins(true, true);
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

static esp_err_t bridge_apply_line_state(bool dtr, bool rts)
{
    /* The DTR/RTS to BOOT/RST transformation follows the auto reset circuitry of the
       ESP development boards, see the ESP USB Bridge schematics. */

    /* defaults for ((dtr && rts) || (!dtr && !rts)) */
    bool rst = true;
    bool boot = true;

    if (!dtr && rts) {
        rst = false;
        boot = true;
    } else if (dtr && !rts) {
        rst = true;
        boot = false;
    }

    esp_timer_stop(s_state_change_timer);  /* may not be running, the return value is not interesting */

    if (dtr && rts) {
        /* The assignment of BOOT=1 and RST=1 is postponed and only done if no other state
           change happens in the meantime. This is a patch for esptool, which emits
           DTR=0 & RTS=1 followed by DTR=1 & RTS=0 with a temporary DTR=1 & RTS=1 in between. */
        ESP_ERROR_CHECK(esp_timer_start_once(s_state_change_timer, 10 * 1000 /*us*/));
        return ESP_OK;
    }

    ESP_LOGI(TAG, "DTR = %d, RTS = %d -> BOOT = %d, RST = %d", dtr, rts, boot, rst);

    serial_handler_set_boot_reset_pins(boot, rst);

    if (!rst) {
        const uint32_t default_baud = 115200;
        esp_err_t err = serial_handler_set_baudrate(default_baud);
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

static void bridge_on_uart_data(const uint8_t *data, size_t len)
{
#if CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE
    bridge_led_serial_rx(true);
    wlink_send(WLINK_MSG_SERIAL, 0, 0, data, len);
    bridge_led_serial_rx(false);
#else
    serial_bridge_target_data_received(data, len);
#endif
}

#endif /* BRIDGE_LOCAL_TARGET */

/* ============================================================== wireless link side */

#if !CONFIG_BRIDGE_ROLE_WIRED

/* ------------------------------------------------------------------- slave side */

#if CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE

typedef struct {
    uint8_t seq;
    uint8_t len;
    uint8_t data[BRIDGE_SWD_PKT_SIZE];
} bridge_swd_req_t;

static QueueHandle_t s_swd_req_q;
static volatile uint8_t s_swd_pending_seq;
static uint8_t s_swd_last_seq;
static bool s_swd_last_valid;
static uint8_t s_swd_last_resp[BRIDGE_SWD_PKT_SIZE];
static size_t s_swd_last_resp_len;

/* Runs in the Wi-Fi task: only queues the request, the DAP engine runs in its own task. */
static void bridge_slave_swd_rx(const wlink_hdr_t *hdr, const uint8_t *payload, size_t len)
{
    if (s_swd_last_valid && hdr->seq == s_swd_last_seq) {
        /* The host missed our answer: reply from the cache instead of running the
           DAP command a second time, which could double-apply a register write. */
        wlink_send(WLINK_MSG_SWD, 0, s_swd_last_seq, s_swd_last_resp, s_swd_last_resp_len);
        return;
    }

    if (hdr->seq == s_swd_pending_seq) {
        /* Already being processed, the response will be sent as soon as it is ready. */
        return;
    }

    if (!s_swd_req_q) {
        ESP_LOGW(TAG, "SWD request queue is not ready yet, dropping seq %u", hdr->seq);
        return;
    }

    bridge_swd_req_t req = {
        .seq = hdr->seq,
        .len = (uint8_t)(len > BRIDGE_SWD_PKT_SIZE ? BRIDGE_SWD_PKT_SIZE : len),
    };
    if (req.len > 0) {
        memcpy(req.data, payload, req.len);
    }

    uint8_t previous_pending = s_swd_pending_seq;
    s_swd_pending_seq = hdr->seq;

    if (xQueueSend(s_swd_req_q, &req, 0) != pdTRUE) {
        s_swd_pending_seq = previous_pending;
        ESP_LOGW(TAG, "SWD request queue full, dropping seq %u", hdr->seq);
    }
}

static void bridge_slave_swd_rx_task(void *arg)
{
    bridge_swd_req_t req;

    while (1) {
        if (xQueueReceive(s_swd_req_q, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        esp_err_t err = debug_probe_process_data(req.data, req.len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "DAP request seq %u rejected: %s", req.seq, esp_err_to_name(err));
        }
    }
}

static void bridge_slave_swd_tx_task(void *arg)
{
    while (1) {
        size_t len = 0;
        uint8_t *buf = debug_probe_get_data_to_send(&len, portMAX_DELAY);
        if (!buf) {
            continue;
        }

        if (len > 0) {
            size_t n = len > BRIDGE_SWD_PKT_SIZE ? BRIDGE_SWD_PKT_SIZE : len;

            /* Cache the answer so a retransmitted request can be served without
               executing the DAP command twice. */
            s_swd_last_seq = s_swd_pending_seq;
            s_swd_last_resp_len = n;
            memcpy(s_swd_last_resp, buf, n);
            s_swd_last_valid = true;

            if (wlink_send(WLINK_MSG_SWD, 0, s_swd_last_seq, s_swd_last_resp, n) != ESP_OK) {
                ESP_LOGW(TAG, "Cannot send DAP response seq %u", s_swd_last_seq);
            }
        }

        debug_probe_free_sent_data(buf);
    }
}

static void bridge_on_link_sercmd(const wlink_hdr_t *hdr, const uint8_t *payload, size_t len)
{
    if (hdr->sub == WLINK_SERCMD_LINE_CODING && len >= sizeof(uint32_t)) {
        uint32_t baud = 0;
        memcpy(&baud, payload, sizeof(baud));
        if (serial_handler_set_baudrate(baud) != ESP_OK) {
            ESP_LOGW(TAG, "Remote baudrate %" PRIu32 " rejected", baud);
        }
    } else if (hdr->sub == WLINK_SERCMD_LINE_STATE && len >= 2) {
        /* The BOOT/RST pins are on this side, so the mapping happens here. */
        bridge_apply_line_state(payload[0] != 0, payload[1] != 0);
    }
}

esp_err_t bridge_wireless_slave_start(void)
{
    esp_err_t err = bridge_uplink_init();
    if (err != ESP_OK) {
        return err;
    }

    err = bridge_probe_init();
    if (err != ESP_OK) {
        return err;
    }

    serial_handler_register_tx_activity_callback(bridge_led_serial_tx);
    serial_handler_register_rx_activity_callback(bridge_led_serial_rx);
    bridge_probe_register_activity_callback(bridge_led_debug);

    s_swd_req_q = xQueueCreate(8, sizeof(bridge_swd_req_t));
    if (!s_swd_req_q) {
        ESP_LOGE(TAG, "Cannot allocate the SWD request queue");
        return ESP_ERR_NO_MEM;
    }

    /* The DAP engine touches dedicated GPIO, which is CPU local, so keep both tasks
       on the core that debug_probe_init() bound the pins to. */
    BaseType_t core = esp_cpu_get_core_id();
    if (xTaskCreatePinnedToCore(bridge_slave_swd_rx_task, "swd_link_rx", 3 * 1024, NULL,
                                DEBUG_PROBE_TASK_PRI, NULL, core) != pdPASS ||
        xTaskCreatePinnedToCore(bridge_slave_swd_tx_task, "swd_link_tx", 3 * 1024, NULL,
                                DEBUG_PROBE_TASK_PRI, NULL, core) != pdPASS) {
        ESP_LOGE(TAG, "Cannot start the SWD link tasks");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Wireless slave ready: UART + CMSIS-DAP SWD over ESP-NOW");
    return ESP_OK;
}

#endif /* CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE */

/* -------------------------------------------------------------------- host side */

#if CONFIG_BRIDGE_ROLE_WIRELESS_HOST

typedef struct {
    uint8_t len;
    uint8_t data[BRIDGE_SWD_PKT_SIZE];
} bridge_host_swd_req_t;

static RingbufHandle_t s_host_swd_sndbuf;
static QueueHandle_t s_host_swd_q;
static SemaphoreHandle_t s_host_swd_resp_sem;
static volatile uint8_t s_host_swd_tx_seq;
static bool s_host_probe_started;

/* Runs in the Wi-Fi task: hand the DAP answer over to the USB IN task. */
static void bridge_host_swd_rx(const wlink_hdr_t *hdr, const uint8_t *payload, size_t len)
{
    if (!s_host_probe_started) {
        return;
    }

    if (hdr->seq != s_host_swd_tx_seq) {
        ESP_LOGD(TAG, "Late DAP response for seq %u (waiting for %u), dropped",
                 hdr->seq, s_host_swd_tx_seq);
        return;
    }

    size_t n = len > BRIDGE_SWD_PKT_SIZE ? BRIDGE_SWD_PKT_SIZE : len;

    /* Non blocking: dropping one answer only costs an OpenOCD retry. */
    if (xRingbufferSend(s_host_swd_sndbuf, payload, n, 0) != pdTRUE) {
        ESP_LOGW(TAG, "SWD response buffer full, dropping %u bytes", (unsigned)n);
        return;
    }

    bridge_led_debug(false);
    xSemaphoreGive(s_host_swd_resp_sem);
}

static void bridge_host_swd_relay_task(void *arg)
{
    bridge_host_swd_req_t req;

    while (1) {
        if (xQueueReceive(s_host_swd_q, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        const uint8_t seq = wlink_next_seq();
        s_host_swd_tx_seq = seq;

        bool delivered = false;
        for (int attempt = 0; attempt < CONFIG_WLINK_SWD_RETRIES; attempt++) {
            if (!wlink_peer_ready()) {
                ESP_LOGW(TAG, "No wireless peer yet, dropping the DAP request");
                break;
            }

            if (wlink_send(WLINK_MSG_SWD, 0, seq, req.data, req.len) != ESP_OK) {
                ESP_LOGW(TAG, "Cannot send the DAP request seq %u", seq);
                continue;
            }

            if (xSemaphoreTake(s_host_swd_resp_sem, pdMS_TO_TICKS(CONFIG_WLINK_SWD_TIMEOUT_MS)) == pdTRUE) {
                delivered = true;
                break;
            }

            ESP_LOGD(TAG, "DAP request seq %u timed out, attempt %d", seq, attempt + 1);
        }

        if (!delivered) {
            /* Let the USB side time out instead of inventing a DAP answer. A late
               response is dropped by the sequence check above. */
            ESP_LOGW(TAG, "DAP request seq %u was not answered, letting the host retry", seq);
            bridge_led_debug(false);
        }
    }
}

#endif /* CONFIG_BRIDGE_ROLE_WIRELESS_HOST */

/* ------------------------------------------------------------ common link handlers */

static void bridge_on_link_serial(const wlink_hdr_t *hdr, const uint8_t *payload, size_t len)
{
#if CONFIG_BRIDGE_ROLE_WIRELESS_HOST
    serial_bridge_target_data_received(payload, len);
#else
    bridge_led_serial_tx(true);
    serial_handler_send_data(payload, len);
    bridge_led_serial_tx(false);
#endif
}

#endif /* !CONFIG_BRIDGE_ROLE_WIRED */

/* ==================================================================== serial uplink */

esp_err_t bridge_uplink_init(void)
{
#if CONFIG_BRIDGE_ROLE_WIRED
    esp_err_t err = bridge_init_state_change_timer();
    if (err != ESP_OK) {
        return err;
    }

    err = serial_handler_init(TRANSPORT_TYPE_UART);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART transport init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = serial_handler_register_data_callback(bridge_on_uart_data);
    if (err != ESP_OK) {
        return err;
    }

    serial_handler_register_tx_activity_callback(bridge_led_serial_tx);
    serial_handler_register_rx_activity_callback(bridge_led_serial_rx);
    return ESP_OK;
#else
 #if CONFIG_BRIDGE_ROLE_WIRELESS_HOST
    const wlink_role_t role = WLINK_ROLE_HOST;
 #else
    const wlink_role_t role = WLINK_ROLE_SLAVE;
 #endif

    esp_err_t err = wlink_init(role);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wireless link init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = wlink_register_rx(WLINK_MSG_SERIAL, bridge_on_link_serial);
    if (err != ESP_OK) {
        return err;
    }

 #if CONFIG_BRIDGE_ROLE_WIRELESS_HOST
    /* The host has no target UART: it only relays to the link. */
    return wlink_register_rx(WLINK_MSG_SWD, bridge_host_swd_rx);
 #else
    err = wlink_register_rx(WLINK_MSG_SWD, bridge_slave_swd_rx);
    if (err != ESP_OK) {
        return err;
    }

    err = wlink_register_rx(WLINK_MSG_SERIAL_CMD, bridge_on_link_sercmd);
    if (err != ESP_OK) {
        return err;
    }

    err = bridge_init_state_change_timer();
    if (err != ESP_OK) {
        return err;
    }

    err = serial_handler_init(TRANSPORT_TYPE_UART);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART transport init failed: %s", esp_err_to_name(err));
        return err;
    }

    return serial_handler_register_data_callback(bridge_on_uart_data);
 #endif
#endif
}

esp_err_t bridge_serial_send(const uint8_t *data, size_t len)
{
#if CONFIG_BRIDGE_ROLE_WIRELESS_HOST
    if (!wlink_peer_ready()) {
        ESP_LOGD(TAG, "No peer yet, %u serial bytes dropped", (unsigned)len);
        return ESP_OK;  /* not an error: the peer may simply not be up yet */
    }
    return wlink_send(WLINK_MSG_SERIAL, 0, 0, data, len);
#else
    return serial_handler_send_data(data, len);
#endif
}

esp_err_t bridge_serial_set_baudrate(uint32_t baud)
{
#if CONFIG_BRIDGE_ROLE_WIRELESS_HOST
    uint8_t payload[sizeof(uint32_t)];
    memcpy(payload, &baud, sizeof(baud));

    if (!wlink_peer_ready()) {
        ESP_LOGD(TAG, "No peer yet, baudrate %" PRIu32 " not forwarded", baud);
        return ESP_OK;
    }
    return wlink_send(WLINK_MSG_SERIAL_CMD, WLINK_SERCMD_LINE_CODING, 0, payload, sizeof(payload));
#else
    return serial_handler_set_baudrate(baud);
#endif
}

esp_err_t bridge_serial_set_line_state(bool dtr, bool rts)
{
#if CONFIG_BRIDGE_ROLE_WIRELESS_HOST
    const uint8_t payload[2] = { dtr ? 1 : 0, rts ? 1 : 0 };

    if (!wlink_peer_ready()) {
        ESP_LOGD(TAG, "No peer yet, DTR/RTS not forwarded");
        return ESP_OK;
    }
    return wlink_send(WLINK_MSG_SERIAL_CMD, WLINK_SERCMD_LINE_STATE, 0, payload, sizeof(payload));
#else
    return bridge_apply_line_state(dtr, rts);
#endif
}

/* =================================================================== debug probe */

#if CONFIG_BRIDGE_ROLE_WIRELESS_HOST

esp_err_t bridge_probe_init(void)
{
    if (s_host_probe_started) {
        return ESP_OK;
    }

    s_host_swd_sndbuf = xRingbufferCreate(8 * 1024, RINGBUF_TYPE_NOSPLIT);
    s_host_swd_q = xQueueCreate(8, sizeof(bridge_host_swd_req_t));
    s_host_swd_resp_sem = xSemaphoreCreateBinary();

    if (!s_host_swd_sndbuf || !s_host_swd_q || !s_host_swd_resp_sem) {
        ESP_LOGE(TAG, "Cannot allocate the SWD relay objects");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(bridge_host_swd_relay_task, "swd_relay", 3 * 1024, NULL,
                    DEBUG_PROBE_TASK_PRI, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Cannot start the SWD relay task");
        return ESP_ERR_NO_MEM;
    }

    s_host_probe_started = true;
    ESP_LOGI(TAG, "SWD relay ready (USB bulk <-> ESP-NOW)");
    return ESP_OK;
}

esp_err_t bridge_probe_process_data(const uint8_t *data, size_t len)
{
    if (len == 0 || len > BRIDGE_SWD_PKT_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_host_probe_started) {
        return ESP_ERR_INVALID_STATE;
    }

    bridge_host_swd_req_t req = { .len = (uint8_t)len };
    memcpy(req.data, data, len);

    if (xQueueSend(s_host_swd_q, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "SWD request queue full, dropping %u bytes", (unsigned)len);
        return ESP_ERR_NO_MEM;
    }

    bridge_led_debug(true);
    return ESP_OK;
}

uint8_t *bridge_probe_get_data_to_send(size_t *len, TickType_t timeout)
{
    if (!s_host_swd_sndbuf) {
        return NULL;
    }
    return xRingbufferReceive(s_host_swd_sndbuf, len, timeout);
}

void bridge_probe_free_sent_data(uint8_t *data)
{
    if (s_host_swd_sndbuf && data) {
        vRingbufferReturnItem(s_host_swd_sndbuf, data);
    }
}

debug_probe_cmd_response_t bridge_probe_handle_command(uint8_t cmd, uint16_t wValue)
{
    /* Vendor control requests cannot be served remotely: the CMSIS-DAP bulk backend
       does not use them, and the firmware caps descriptor lives in the device. */
    (void)cmd;
    (void)wValue;
    return (debug_probe_cmd_response_t) { .success = false };
}

int bridge_probe_get_proto_caps(void *dest)
{
    (void)dest;
    return 0;
}

void bridge_probe_register_activity_callback(debug_activity_notify_cb_t callback)
{
    (void)callback;  /* the relay drives LED_JTAG itself */
}

void bridge_probe_handle_esp32_tdi_bootstrapping(bool rebooting)
{
    (void)rebooting;  /* the JTAG/SWD pins are on the slave */
}

#else /* wired or wireless slave: the probe engine is local */

esp_err_t bridge_probe_init(void)
{
    return debug_probe_init();
}

esp_err_t bridge_probe_process_data(const uint8_t *data, size_t len)
{
    return debug_probe_process_data(data, len);
}

uint8_t *bridge_probe_get_data_to_send(size_t *len, TickType_t timeout)
{
    return debug_probe_get_data_to_send(len, timeout);
}

void bridge_probe_free_sent_data(uint8_t *data)
{
    debug_probe_free_sent_data(data);
}

debug_probe_cmd_response_t bridge_probe_handle_command(uint8_t cmd, uint16_t wValue)
{
    return debug_probe_handle_command(cmd, wValue);
}

int bridge_probe_get_proto_caps(void *dest)
{
    return debug_probe_get_proto_caps(dest);
}

void bridge_probe_register_activity_callback(debug_activity_notify_cb_t callback)
{
    debug_probe_register_activity_callback(callback);
}

void bridge_probe_handle_esp32_tdi_bootstrapping(bool rebooting)
{
    debug_probe_handle_esp32_tdi_bootstrapping(rebooting);
}

#endif
