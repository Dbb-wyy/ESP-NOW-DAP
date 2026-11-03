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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"

#include "bridge_glue.h"
#include "serial_bridge.h"
#include "serial_handler.h"
#include "led_rgb.h"

#if !CONFIG_BRIDGE_ROLE_WIRED
#include "wireless_link.h"
#endif

static const char *TAG = "bridge_glue";

/* Roles that own the target pins directly. */
#define BRIDGE_LOCAL_TARGET (!CONFIG_BRIDGE_ROLE_WIRELESS_HOST)

/* The wireless roles tunnel CMSIS-DAP, which only exists in the SWD build. */
#if !CONFIG_BRIDGE_ROLE_WIRED && defined(CONFIG_DEBUG_PROBE_IFACE_JTAG)
#error "The wireless bridge roles require 'END-config -> Role configuration -> Debug interface -> CMSIS-DAP SWD' (CONFIG_DEBUG_PROBE_IFACE_SWD)."
#endif

/* Maximum DAP packet on the wire, matches DAP_PACKET_SIZE / EUB_VENDORD_EPSIZE. */
#define BRIDGE_SWD_PKT_SIZE 64

/*
 * Depth of the slave SWD request queue, and of the sequence list that tracks the requests
 * which were accepted but not answered yet. The list has to cover the queue plus whatever
 * the DAP engine still holds (1024 byte ring buffers holding 64 byte packets); the host is
 * stop-and-wait, so in practice one or two entries are in use.
 */
#define BRIDGE_SWD_REQ_QUEUE_DEPTH 8
#define BRIDGE_SWD_SEQ_FIFO_DEPTH 32

/* How often the host relay re-checks the link while it waits for the peer. */
#define BRIDGE_PEER_POLL_MS 100

/*
 * Per packet logging on the SWD path. It stays at debug level by default, because a
 * debug session can send hundreds of DAP commands per second and the serial console is
 * far slower than that; CONFIG_WLINK_SWD_VERBOSE_LOG promotes it to info level when the
 * path has to be diagnosed. State changes and anomalies are logged unconditionally.
 */
#if CONFIG_WLINK_SWD_VERBOSE_LOG
#define BRIDGE_SWD_PKT_LOG(fmt, ...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#else
#define BRIDGE_SWD_PKT_LOG(fmt, ...) ESP_LOGD(TAG, fmt, ##__VA_ARGS__)
#endif

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
    led_rgb_notify_activity(LED_RGB_ACT_SERIAL);

#if CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE
    wlink_send(WLINK_MSG_SERIAL, 0, 0, data, len);
#else
    serial_bridge_target_data_received(data, len);
#endif
}

#endif /* BRIDGE_LOCAL_TARGET */

/* ============================================================== wireless link side */

#if !CONFIG_BRIDGE_ROLE_WIRED

/*
 * The CMSIS-DAP bulk backend carries no report ID, so the command is always the
 * first payload byte. Watching it gives the status LED a session edge to latch
 * on instead of raw traffic: a debugger sitting on a halted target sends
 * nothing at all for minutes and the blue state still has to stay on.
 */
#define BRIDGE_DAP_ID_CONNECT    0x02u
#define BRIDGE_DAP_ID_DISCONNECT 0x03u

static void bridge_dap_track_session(const uint8_t *data, size_t len) {
    if (len == 0) {
        return;
    }

    if (data[0] == BRIDGE_DAP_ID_CONNECT) {
        led_rgb_notify_swd_session(true);
    } else if (data[0] == BRIDGE_DAP_ID_DISCONNECT) {
        led_rgb_notify_swd_session(false);
    }
}

/* ------------------------------------------------------------------- slave side */

#if CONFIG_BRIDGE_ROLE_WIRELESS_SLAVE

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
static void bridge_slave_swd_rx(const wlink_hdr_t *hdr, const uint8_t *payload, size_t len)
{
    led_rgb_notify_activity(LED_RGB_ACT_SWD);

    if (len == 0 || len > BRIDGE_SWD_PKT_SIZE) {
        ESP_LOGW(TAG, "DAP request seq %u has an unusable length (%u bytes), dropped",
                 hdr->seq, (unsigned)len);
        return;
    }

    bridge_dap_track_session(payload, len);

    /* Copy the cached answer under the lock the sender uses, so a response that lands
       meanwhile cannot tear the sequence number away from its payload. */
    uint8_t cached[BRIDGE_SWD_PKT_SIZE];
    size_t cached_len = 0;
    bool have_cached = false;

    portENTER_CRITICAL(&s_swd_seq_mux);
    if (s_swd_last_valid && hdr->seq == s_swd_last_seq) {
        cached_len = s_swd_last_resp_len;
        memcpy(cached, s_swd_last_resp, cached_len);
        have_cached = true;
    }
    portEXIT_CRITICAL(&s_swd_seq_mux);

    if (have_cached) {
        /* The host missed our answer: reply from the cache instead of running the
           DAP command a second time, which could double-apply a register write. */
        BRIDGE_SWD_PKT_LOG("DAP request seq %u served from the cache (retransmit)", hdr->seq);
        wlink_send(WLINK_MSG_SWD, 0, hdr->seq, cached, cached_len);
        return;
    }

    bridge_swd_req_t req = {
        .seq = hdr->seq,
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
    duplicate = bridge_swd_seq_pending(hdr->seq);
    if (!duplicate && s_swd_req_q && bridge_swd_seq_push(hdr->seq)) {
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
        BRIDGE_SWD_PKT_LOG("DAP request seq %u is already in flight, ignored", hdr->seq);
        return;
    }

    if (!queued) {
        ESP_LOGW(TAG, "SWD request seq %u dropped: request queue not ready or full", hdr->seq);
        return;
    }

    BRIDGE_SWD_PKT_LOG("DAP request seq %u received over the link (%u bytes), queued",
                       hdr->seq, (unsigned)req.len);
}

static void bridge_slave_swd_rx_task(void *arg)
{
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
        } else if (wlink_send(WLINK_MSG_SWD, 0, seq, s_swd_last_resp, n) != ESP_OK) {
            ESP_LOGW(TAG, "Cannot send DAP response seq %u", seq);
        } else {
            BRIDGE_SWD_PKT_LOG("DAP response seq %u sent over the link (%u bytes)",
                               seq, (unsigned)n);
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

    /* The request queue is created by bridge_uplink_init() right before it registers the
       receive callback, so it cannot be missing here. */

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
    led_rgb_notify_activity(LED_RGB_ACT_SWD);

    if (!s_host_probe_started) {
        return;
    }

    if (hdr->seq != s_host_swd_tx_seq) {
        BRIDGE_SWD_PKT_LOG("Late DAP response for seq %u (waiting for %u), dropped",
                           hdr->seq, s_host_swd_tx_seq);
        return;
    }

    size_t n = len > BRIDGE_SWD_PKT_SIZE ? BRIDGE_SWD_PKT_SIZE : len;

    /* Non blocking: dropping one answer only costs an OpenOCD retry. */
    if (xRingbufferSend(s_host_swd_sndbuf, payload, n, 0) != pdTRUE) {
        ESP_LOGW(TAG, "SWD response buffer full, dropping %u bytes", (unsigned)n);
        return;
    }

    BRIDGE_SWD_PKT_LOG("DAP response seq %u arrived over the link (%u bytes)", hdr->seq, (unsigned)n);
    xSemaphoreGive(s_host_swd_resp_sem);
}

static void bridge_host_swd_relay_task(void *arg)
{
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

        led_rgb_notify_activity(LED_RGB_ACT_SWD);

        /*
         * Even without a peer the frame is still sent, as a broadcast, which the slave
         * receives as well. So fall through here rather than dropping the request.
         */
        bool delivered = false;
        for (int attempt = 0; attempt < CONFIG_WLINK_SWD_RETRIES; attempt++) {
            if (wlink_send(WLINK_MSG_SWD, 0, seq, req.data, req.len) != ESP_OK) {
                ESP_LOGW(TAG, "Cannot send the DAP request seq %u", seq);
                continue;
            }

            BRIDGE_SWD_PKT_LOG("DAP request seq %u sent (%u bytes, attempt %d%s)",
                               seq, (unsigned)req.len, attempt + 1,
                               wlink_peer_ready() ? "" : ", broadcast, link unpaired");

            if (xSemaphoreTake(s_host_swd_resp_sem, pdMS_TO_TICKS(CONFIG_WLINK_SWD_TIMEOUT_MS)) == pdTRUE) {
                delivered = true;
                break;
            }

            BRIDGE_SWD_PKT_LOG("DAP request seq %u timed out, attempt %d", seq, attempt + 1);
        }

        if (!delivered) {
            /* Let the USB side time out instead of inventing a DAP answer. A late
               response is dropped by the sequence check above. */
            ESP_LOGW(TAG, "DAP request seq %u was not answered after %d attempts, link is %s",
                     seq, CONFIG_WLINK_SWD_RETRIES,
                     wlink_peer_ready() ? "paired" : "still unpaired");
        }
    }
}

#endif /* CONFIG_BRIDGE_ROLE_WIRELESS_HOST */

/* ------------------------------------------------------------ common link handlers */

static void bridge_on_link_serial(const wlink_hdr_t *hdr, const uint8_t *payload, size_t len)
{
    led_rgb_notify_activity(LED_RGB_ACT_SERIAL);

#if CONFIG_BRIDGE_ROLE_WIRELESS_HOST
    serial_bridge_target_data_received(payload, len);
#else
    serial_handler_send_data(payload, len);
#endif
}

/*
 * The status LED pulls the link state instead of being told about it, so nothing has to
 * hook into the ESP-NOW callbacks. wlink_link_up() is a passive observation with its own
 * timeout: wlink_peer_ready() must keep meaning "the MAC is known", because the SWD
 * stop-and-wait logic depends on that and would otherwise start flapping.
 */
static const led_rgb_link_ops_t k_led_link_ops = {
    .link_up = wlink_link_up,
};

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

    /* From here on the status LED can tell an unpaired link from a paired one. */
    led_rgb_set_link_ops(&k_led_link_ops);

    err = wlink_register_rx(WLINK_MSG_SERIAL, bridge_on_link_serial);
    if (err != ESP_OK) {
        return err;
    }

 #if CONFIG_BRIDGE_ROLE_WIRELESS_HOST
    /* The host has no target UART: it only relays to the link. */
    return wlink_register_rx(WLINK_MSG_SWD, bridge_host_swd_rx);
 #else
    /*
     * The queue has to exist before the receive callback is registered below: a request
     * that arrives in between would be dropped, which is exactly the "first SWD access
     * after a fresh power-up fails" symptom.
     */
    if (!s_swd_req_q) {
        s_swd_req_q = xQueueCreate(BRIDGE_SWD_REQ_QUEUE_DEPTH, sizeof(bridge_swd_req_t));
        if (!s_swd_req_q) {
            ESP_LOGE(TAG, "Cannot allocate the SWD request queue");
            return ESP_ERR_NO_MEM;
        }
    }

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
    led_rgb_notify_activity(LED_RGB_ACT_SERIAL);

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

void bridge_probe_handle_esp32_tdi_bootstrapping(bool rebooting)
{
    debug_probe_handle_esp32_tdi_bootstrapping(rebooting);
}

#endif
