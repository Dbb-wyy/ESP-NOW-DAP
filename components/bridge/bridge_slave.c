/*
 * SPDX-FileCopyrightText: 2026 Dbb
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Wireless slave role: the target side. The ESP-NOW link is the only host
 * interface; the DAP engine, the target UART and the BOOT/RST lines all live
 * here. Requests are answered in order and cached by sequence number, so a
 * retransmission never runs a DAP command twice.
 */

#include <inttypes.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "bridge_priv.h"
#include "debug_probe.h"
#include "status_led.h"
#include "target_uart.h"

#if BRIDGE_ROLE_ID == BRIDGE_ROLE_SLAVE_ID

static const char *TAG = "bridge";

typedef struct {
    uint8_t seq;
    uint8_t len;
    uint8_t data[BRIDGE_SWD_PKT_SIZE];
} bridge_swd_req_t;

static QueueHandle_t s_swd_req_q;

/*
 * Sequence numbers of the requests that were accepted but not answered yet, oldest first.
 * The DAP engine answers every request with exactly one response, in request order, so
 * popping one entry per response keeps each answer tied to the request that produced it
 * -- even when the host already gave up on an older request and sent the next one.
 *
 * Guarded by s_swd_seq_mux: the Wi-Fi callback and both link tasks touch it, and all of
 * them run in task context, so a spinlock is enough.
 */
static uint8_t s_swd_seq_fifo[BRIDGE_SWD_SEQ_FIFO_DEPTH];
static size_t s_swd_seq_fifo_head;
static size_t s_swd_seq_fifo_count;
static portMUX_TYPE s_swd_seq_mux = portMUX_INITIALIZER_UNLOCKED;

/* Last answer, so a retransmitted request is served without running the DAP command twice. */
static uint8_t s_swd_last_seq;
static bool s_swd_last_valid;
static uint8_t s_swd_last_resp[BRIDGE_SWD_PKT_SIZE];
static size_t s_swd_last_resp_len;

/* The s_swd_seq_fifo helpers require s_swd_seq_mux to be held. */
static bool bridge_swd_seq_pending(uint8_t seq)
{
    for (size_t i = 0; i < s_swd_seq_fifo_count; i++) {
        size_t slot = (s_swd_seq_fifo_head + i) % BRIDGE_SWD_SEQ_FIFO_DEPTH;
        if (s_swd_seq_fifo[slot] == seq) {
            return true;
        }
    }
    return false;
}

static bool bridge_swd_seq_push(uint8_t seq)
{
    if (s_swd_seq_fifo_count >= BRIDGE_SWD_SEQ_FIFO_DEPTH) {
        return false;
    }

    size_t slot = (s_swd_seq_fifo_head + s_swd_seq_fifo_count) % BRIDGE_SWD_SEQ_FIFO_DEPTH;
    s_swd_seq_fifo[slot] = seq;
    s_swd_seq_fifo_count++;
    return true;
}

static bool bridge_swd_seq_pop(uint8_t *seq)
{
    if (s_swd_seq_fifo_count == 0) {
        return false;
    }

    if (seq) {
        *seq = s_swd_seq_fifo[s_swd_seq_fifo_head];
    }
    s_swd_seq_fifo_head = (s_swd_seq_fifo_head + 1) % BRIDGE_SWD_SEQ_FIFO_DEPTH;
    s_swd_seq_fifo_count--;
    return true;
}

/* Undo the last push, for a request that turned out not to be queued after all. */
static void bridge_swd_seq_drop_last(void)
{
    if (s_swd_seq_fifo_count > 0) {
        s_swd_seq_fifo_count--;
    }
}

/* Runs in the Wi-Fi task: only queues the request, the DAP engine runs in its own task. */
static void bridge_slave_swd_rx(wlink_channel_t channel, uint8_t sub, uint8_t seq,
                                const uint8_t *payload, size_t len)
{
    (void)channel;
    (void)sub;
    status_led_notify_activity(STATUS_LED_ACT_SWD);

    if (len == 0 || len > BRIDGE_SWD_PKT_SIZE) {
        ESP_LOGW(TAG, "DAP request seq %u has an unusable length (%u bytes), dropped",
                 seq, (unsigned)len);
        return;
    }

    bridge_dap_track_session(payload, len);

    /* Copy the cached answer under the lock the sender uses, so a response that lands
       meanwhile cannot tear the sequence number away from its payload. */
    uint8_t cached[BRIDGE_SWD_PKT_SIZE];
    size_t cached_len = 0;
    bool have_cached = false;

    portENTER_CRITICAL(&s_swd_seq_mux);
    if (s_swd_last_valid && seq == s_swd_last_seq) {
        cached_len = s_swd_last_resp_len;
        memcpy(cached, s_swd_last_resp, cached_len);
        have_cached = true;
    }
    portEXIT_CRITICAL(&s_swd_seq_mux);

    if (have_cached) {
        /* The host missed our answer: reply from the cache instead of running the
           DAP command a second time, which could double-apply a register write. */
        BRIDGE_SWD_PKT_LOG("DAP request seq %u served from the cache (retransmit)", seq);
        wlink_send(WLINK_CHANNEL_SWD, 0, seq, cached, cached_len);
        return;
    }

    bridge_swd_req_t req = {
        .seq = seq,
        .len = (uint8_t)len,
    };
    memcpy(req.data, payload, req.len);

    /*
     * Accepting the request and recording its sequence number are one step: the same seq
     * must never be queued twice (the DAP command would run twice), and a request that is
     * dropped below must not leave a sequence entry behind.
     */
    bool duplicate = false;
    bool queued = false;

    portENTER_CRITICAL(&s_swd_seq_mux);
    duplicate = bridge_swd_seq_pending(seq);
    if (!duplicate && s_swd_req_q && bridge_swd_seq_push(seq)) {
        if (xQueueSend(s_swd_req_q, &req, 0) != pdTRUE) {
            bridge_swd_seq_drop_last();
        } else {
            queued = true;
        }
    }
    portEXIT_CRITICAL(&s_swd_seq_mux);

    if (duplicate) {
        /* Already accepted: the answer to it is on its way, and once it is cached a
           retransmission is served from there. */
        BRIDGE_SWD_PKT_LOG("DAP request seq %u is already in flight, ignored", seq);
        return;
    }

    if (!queued) {
        ESP_LOGW(TAG, "SWD request seq %u dropped: request queue not ready or full", seq);
        return;
    }

    BRIDGE_SWD_PKT_LOG("DAP request seq %u received over the link (%u bytes), queued",
                       seq, (unsigned)req.len);
}

static void bridge_slave_swd_rx_task(void *arg)
{
    (void)arg;
    bridge_swd_req_t req;

    while (1) {
        if (xQueueReceive(s_swd_req_q, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        esp_err_t err = debug_probe_process_data(req.data, req.len);
        if (err == ESP_OK) {
            continue;
        }

        /*
         * The DAP engine will never answer this request, so its sequence entry has to go
         * as well: requests are taken in accept order, which makes this one the oldest
         * pending entry, and leaving it behind would make the next answer carry its number.
         */
        uint8_t dropped_seq = 0;
        bool dropped = false;

        portENTER_CRITICAL(&s_swd_seq_mux);
        dropped = bridge_swd_seq_pop(&dropped_seq);
        portEXIT_CRITICAL(&s_swd_seq_mux);

        if (!dropped) {
            ESP_LOGW(TAG, "DAP request seq %u rejected (%s), no sequence entry to drop",
                     req.seq, esp_err_to_name(err));
        } else if (dropped_seq != req.seq) {
            ESP_LOGW(TAG, "DAP request seq %u rejected (%s), sequence list out of sync (head %u)",
                     req.seq, esp_err_to_name(err), dropped_seq);
        } else {
            ESP_LOGW(TAG, "DAP request seq %u rejected (%s), request dropped",
                     req.seq, esp_err_to_name(err));
        }
    }
}

static void bridge_slave_swd_tx_task(void *arg)
{
    (void)arg;
    while (1) {
        size_t len = 0;
        uint8_t *buf = debug_probe_get_data_to_send(&len, portMAX_DELAY);
        if (!buf) {
            continue;
        }

        /*
         * One answer per request and in request order, so the oldest pending sequence
         * number is the one this answer belongs to. Popping it here -- instead of reading
         * a "request currently in flight" variable -- is what keeps the answer correct when
         * the host already gave up on that request and sent the next one.
         */
        uint8_t seq = 0;
        size_t n = len > BRIDGE_SWD_PKT_SIZE ? BRIDGE_SWD_PKT_SIZE : len;
        bool have_seq = false;

        portENTER_CRITICAL(&s_swd_seq_mux);
        have_seq = bridge_swd_seq_pop(&seq);
        if (have_seq && n > 0) {
            /* Cache the answer so a retransmitted request can be served without
               executing the DAP command twice. */
            s_swd_last_seq = seq;
            s_swd_last_resp_len = n;
            memcpy(s_swd_last_resp, buf, n);
            s_swd_last_valid = true;
        }
        portEXIT_CRITICAL(&s_swd_seq_mux);

        if (!have_seq) {
            ESP_LOGE(TAG, "DAP response of %u bytes has no pending request, dropped",
                     (unsigned)len);
        } else if (n == 0) {
            BRIDGE_SWD_PKT_LOG("DAP response seq %u is empty, nothing to send", seq);
        } else if (wlink_send(WLINK_CHANNEL_SWD, 0, seq, s_swd_last_resp, n) != ESP_OK) {
            ESP_LOGW(TAG, "Cannot send DAP response seq %u", seq);
        } else {
            BRIDGE_SWD_PKT_LOG("DAP response seq %u sent over the link (%u bytes)",
                               seq, (unsigned)n);
        }

        debug_probe_free_sent_data(buf);
    }
}

static void bridge_on_link_sercmd(wlink_channel_t channel, uint8_t sub, uint8_t seq,
                                  const uint8_t *payload, size_t len)
{
    (void)channel;
    (void)seq;
    if (sub == BRIDGE_SERCMD_LINE_CODING && len >= sizeof(uint32_t)) {
        uint32_t baud = 0;
        memcpy(&baud, payload, sizeof(baud));
        if (target_uart_set_baudrate(baud) != ESP_OK) {
            ESP_LOGW(TAG, "Remote baudrate %" PRIu32 " rejected", baud);
        }
    } else if (sub == BRIDGE_SERCMD_LINE_STATE && len >= 2) {
        /* The BOOT/RST pins are on this side, so the mapping happens here. */
        bridge_apply_line_state(payload[0] != 0, payload[1] != 0);
    }
}

esp_err_t bridge_slave_init(void)
{
    esp_err_t err = wlink_init(WLINK_ROLE_SLAVE);
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

    /*
     * The queue has to exist before the receive callback is registered: a request that
     * arrives in between would be dropped, which is exactly the "first SWD access after
     * a fresh power-up fails" symptom.
     */
    if (!s_swd_req_q) {
        s_swd_req_q = xQueueCreate(BRIDGE_SWD_REQ_QUEUE_DEPTH, sizeof(bridge_swd_req_t));
        if (!s_swd_req_q) {
            ESP_LOGE(TAG, "Cannot allocate the SWD request queue");
            return ESP_ERR_NO_MEM;
        }
    }

    err = wlink_register_rx(WLINK_CHANNEL_SWD, bridge_slave_swd_rx);
    if (err != ESP_OK) {
        return err;
    }

    err = wlink_register_rx(WLINK_CHANNEL_SERIAL_CMD, bridge_on_link_sercmd);
    if (err != ESP_OK) {
        return err;
    }

    err = bridge_local_target_start();
    if (err != ESP_OK) {
        return err;
    }

    err = bridge_probe_init();
    if (err != ESP_OK) {
        return err;
    }

    /* The DAP engine touches dedicated GPIO, which is CPU local, so keep both tasks
       on the core that debug_probe_init() bound the pins to. */
    BaseType_t core = esp_cpu_get_core_id();
    if (xTaskCreatePinnedToCore(bridge_slave_swd_rx_task, "swd_link_rx", 3 * 1024, NULL,
                                BRIDGE_TASK_PRI, NULL, core) != pdPASS ||
        xTaskCreatePinnedToCore(bridge_slave_swd_tx_task, "swd_link_tx", 3 * 1024, NULL,
                                BRIDGE_TASK_PRI, NULL, core) != pdPASS) {
        ESP_LOGE(TAG, "Cannot start the SWD link tasks");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Wireless slave ready: UART + CMSIS-DAP SWD over ESP-NOW");
    return ESP_OK;
}

#endif /* BRIDGE_ROLE_ID == BRIDGE_ROLE_SLAVE_ID */
