/*
 * SPDX-FileCopyrightText: 2026 Dbb
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Wireless host role: the PC side. The ESP-NOW link replaces the target UART,
 * the SWD path is relayed between the USB vendor bulk endpoint and the link, and
 * the DAP engine itself lives on the slave.
 */

#include <inttypes.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"

#include "bridge_priv.h"
#include "status_led.h"
#include "usb_device.h"

#if BRIDGE_ROLE_ID == BRIDGE_ROLE_HOST_ID

static const char *TAG = "bridge";

typedef struct {
    uint8_t len;
    uint8_t data[BRIDGE_SWD_PKT_SIZE];
} bridge_host_swd_req_t;

static RingbufHandle_t s_host_swd_sndbuf;
static QueueHandle_t s_host_swd_q;
static SemaphoreHandle_t s_host_swd_resp_sem;
static volatile uint8_t s_host_swd_tx_seq;
static bool s_host_probe_started;

/* Set when a request was abandoned: its late answer must not reach the host, which by
   then waits for the answer of a newer request. */
static uint8_t s_host_swd_dropped_seq;
static bool s_host_swd_drop_valid;

/* Runs in the Wi-Fi task: hand the DAP answer over to the USB IN task. */
static void bridge_host_swd_rx(wlink_channel_t channel, uint8_t sub, uint8_t seq,
                               const uint8_t *payload, size_t len)
{
    (void)channel;
    (void)sub;
    status_led_notify_activity(STATUS_LED_ACT_SWD);

    if (!s_host_probe_started) {
        return;
    }

    if (s_host_swd_drop_valid && seq == s_host_swd_dropped_seq) {
        /* Answer to the request we already reported as failed: the host has moved on. */
        ESP_LOGW(TAG, "DAP response seq %u arrived after the request was abandoned, dropped",
                 (unsigned)seq);
        return;
    }

    if (seq != s_host_swd_tx_seq) {
        ESP_LOGW(TAG, "Late DAP response for seq %u (waiting for %u), dropped",
                 seq, (unsigned)s_host_swd_tx_seq);
        return;
    }

    size_t n = len > BRIDGE_SWD_PKT_SIZE ? BRIDGE_SWD_PKT_SIZE : len;

    /* Non blocking: dropping one answer only costs an OpenOCD retry. */
    if (xRingbufferSend(s_host_swd_sndbuf, payload, n, 0) != pdTRUE) {
        ESP_LOGW(TAG, "SWD response buffer full, dropping %u bytes", (unsigned)n);
        return;
    }

    BRIDGE_SWD_PKT_LOG("DAP response seq %u arrived over the link (%u bytes)", seq, (unsigned)n);
    xSemaphoreGive(s_host_swd_resp_sem);
}

static void bridge_host_swd_relay_task(void *arg)
{
    (void)arg;
    bridge_host_swd_req_t req;
    bool reported_wait = false;

    while (1) {
        if (xQueueReceive(s_host_swd_q, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /*
         * A board that was just reset does not know its peer yet, while the host tool
         * sends its very first command as soon as the USB device enumerates and gives
         * up if that command goes unanswered. So wait for the link to pair instead of
         * dropping the request: a paired link lets the request go out as a unicast
         * frame, which ESP-NOW acknowledges and retries on its own.
         */
        if (!wlink_peer_ready()) {
            if (!reported_wait) {
                ESP_LOGI(TAG, "Link is not paired yet, waiting up to %d ms for the peer",
                         CONFIG_WLINK_PEER_WAIT_MS);
                reported_wait = true;
            }

            int waited = 0;
            while (waited < CONFIG_WLINK_PEER_WAIT_MS && !wlink_peer_ready()) {
                vTaskDelay(pdMS_TO_TICKS(BRIDGE_PEER_POLL_MS));
                waited += BRIDGE_PEER_POLL_MS;
            }

            if (wlink_peer_ready()) {
                ESP_LOGI(TAG, "Link paired again after waiting %d ms", waited);
                reported_wait = false;
            }
        }

        const uint8_t seq = wlink_next_seq();
        s_host_swd_tx_seq = seq;

        status_led_notify_activity(STATUS_LED_ACT_SWD);

        /*
         * Even without a peer the frame is still sent, as a broadcast, which the slave
         * receives as well. So fall through here rather than dropping the request.
         */
        bool delivered = false;
        int64_t sent_at = esp_timer_get_time();
        for (int attempt = 0; attempt < CONFIG_WLINK_SWD_RETRIES; attempt++) {
            if (wlink_send(WLINK_CHANNEL_SWD, 0, seq, req.data, req.len) != ESP_OK) {
                ESP_LOGW(TAG, "Cannot send the DAP request seq %u", seq);
                continue;
            }

            BRIDGE_SWD_PKT_LOG("DAP request seq %u sent (%u bytes, attempt %d%s)",
                               seq, (unsigned)req.len, attempt + 1,
                               wlink_peer_ready() ? "" : ", broadcast, link unpaired");

            if (xSemaphoreTake(s_host_swd_resp_sem, pdMS_TO_TICKS(CONFIG_WLINK_SWD_TIMEOUT_MS)) == pdTRUE) {
                delivered = true;
                int64_t took_ms = (esp_timer_get_time() - sent_at) / 1000;
                if (took_ms >= BRIDGE_SWD_SLOW_MS) {
                    ESP_LOGW(TAG, "DAP request seq %u (cmd 0x%02x, %u bytes) took %" PRId64
                             " ms to be answered", seq, req.len > 0 ? req.data[0] : 0,
                             (unsigned)req.len, took_ms);
                }
                break;
            }

            BRIDGE_SWD_PKT_LOG("DAP request seq %u timed out, attempt %d", seq, attempt + 1);
        }

        if (!delivered) {
            /*
             * Never invent a DAP answer, but never leave the host hanging either: a bulk
             * IN read only completes when the device sends data, so report the failure at
             * the USB level by stalling the endpoint. The host tool then sees a transfer
             * error within a second and can retry, instead of waiting forever.
             */
            s_host_swd_dropped_seq = seq;
            s_host_swd_drop_valid = true;

            /* An answer cannot be queued after this point, but clear whatever a previous
               request left behind so the next answer is read on its own. */
            while (xSemaphoreTake(s_host_swd_resp_sem, 0) == pdTRUE) {
            }

            usb_device_vendor_abort_pending();

            ESP_LOGW(TAG, "DAP request seq %u was not answered after %d attempts, link is %s, "
                          "reported to the USB host as a transfer error",
                     seq, CONFIG_WLINK_SWD_RETRIES,
                     wlink_peer_ready() ? "paired" : "still unpaired");
        } else {
            s_host_swd_drop_valid = false;
        }
    }
}

esp_err_t bridge_host_init(void)
{
    esp_err_t err = wlink_init(WLINK_ROLE_HOST);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wireless link init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* From here on the status LED can tell an unpaired link from a paired one. */
    status_led_set_link_ops(&k_bridge_led_link_ops);

    err = wlink_register_rx(WLINK_CHANNEL_SERIAL, bridge_on_link_serial);
    if (err != ESP_OK) {
        return err;
    }

    /* The host has no target UART: it only relays to the link. */
    return wlink_register_rx(WLINK_CHANNEL_SWD, bridge_host_swd_rx);
}

/* =================================================================== debug probe */

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
                    BRIDGE_TASK_PRI, NULL) != pdPASS) {
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

    bridge_dap_track_session(data, len);

    bridge_host_swd_req_t req = { .len = (uint8_t)len };
    memcpy(req.data, data, len);

    if (xQueueSend(s_host_swd_q, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "SWD request queue full, dropping %u bytes", (unsigned)len);
        return ESP_ERR_NO_MEM;
    }

    BRIDGE_SWD_PKT_LOG("DAP request from USB queued (%u bytes)", (unsigned)len);
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

esp_err_t bridge_probe_handle_command(uint8_t cmd, uint16_t wValue,
                                      const uint8_t **out_data, size_t *out_len)
{
    /* Vendor control requests cannot be served remotely: the CMSIS-DAP bulk backend
       does not use them, and the firmware caps descriptor lives in the device. */
    (void)cmd;
    (void)wValue;
    if (out_data != NULL) {
        *out_data = NULL;
    }
    if (out_len != NULL) {
        *out_len = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

int bridge_probe_get_proto_caps(void *dest)
{
    (void)dest;
    return 0;
}

void bridge_probe_handle_esp32_tdi_bootstrapping(bool rebooting)
{
    (void)rebooting;  /* the JTAG/SWD pins are on the slave */
}

#endif /* BRIDGE_ROLE_ID == BRIDGE_ROLE_HOST_ID */
