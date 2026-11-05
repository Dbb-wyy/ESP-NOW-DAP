/*
 * SPDX-FileCopyrightText: 2026 Dbb
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "wireless_link.h"
#include "wireless_link_priv.h"

static const char *TAG = "wlink";

/* Human readable message type for internal logs only: the type enum is private. */
static const char *wlink_msg_type_str(wlink_msg_type_t type);

static const uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static wlink_role_t s_role = WLINK_ROLE_NONE;
static wlink_rx_cb_t s_rx_cb[WLINK_CHANNEL_COUNT];
static SemaphoreHandle_t s_tx_mutex;

static uint8_t s_peer_mac[ESP_NOW_ETH_ALEN];
static volatile bool s_peer_ready;
static uint8_t s_seq;
static esp_timer_handle_t s_beacon_timer;

/*
 * Liveness of the link, kept only for the status LED.
 *
 * s_peer_ready answers "do we know where the peer is", which stays true across a long
 * silence and drives the SWD retransmission. The LED needs the other question - "was the
 * peer heard from recently" - so the receive callback stamps every frame that comes from
 * the peer MAC and the timeout is applied when somebody asks. 0 means "never heard from";
 * real stamps are ORed with 1 so they can never collide with it.
 */
static volatile uint32_t s_peer_last_seen_ms;

static uint32_t wlink_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

#if CONFIG_WLINK_DEBUG_LINK_TEST
static esp_err_t wlink_start_debug_task(void);
#endif

static esp_err_t wlink_add_peer(const uint8_t mac[ESP_NOW_ETH_ALEN])
{
    esp_now_peer_info_t peer = {
        .channel = CONFIG_WLINK_CHANNEL,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, mac, ESP_NOW_ETH_ALEN);

    esp_err_t err = esp_now_add_peer(&peer);
    if (err == ESP_ERR_ESPNOW_EXIST) {
        return ESP_OK;
    }
    return err;
}

static void wlink_learn_peer(const uint8_t mac[ESP_NOW_ETH_ALEN])
{
    if (s_peer_ready) {
        return;
    }

    if (wlink_add_peer(mac) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot add peer " MACSTR, MAC2STR(mac));
        return;
    }

    memcpy(s_peer_mac, mac, ESP_NOW_ETH_ALEN);
    s_peer_ready = true;
    s_peer_last_seen_ms = wlink_now_ms() | 1u;

    /*
     * Keep beaconing once the peer is known.
     *
     * A peer that reboots forgets us, and it can only rediscover us from a frame
     * that we send. Stopping the beacons here left such a peer unpaired until we
     * happened to have payload for it, which is exactly the case right after a
     * reset when nothing is being typed into the serial port yet.
     *
     * One small broadcast frame per second costs nothing next to the traffic the
     * link carries once it is in use, and it makes either side recover from a
     * reset on its own.
     */
    ESP_LOGI(TAG, "Peer discovered: " MACSTR " (peer role: %s), still beaconing every %d ms",
             MAC2STR(s_peer_mac), s_role == WLINK_ROLE_HOST ? "slave" : "host",
             WLINK_BEACON_INTERVAL_MS);
}

/* Runs in the Wi-Fi task: this must not block. */
static void wlink_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < WLINK_HDR_LEN) {
        return;
    }

    const wlink_hdr_t *hdr = (const wlink_hdr_t *)data;

    /* Only pair with the opposite role, so a second host cannot hijack a slave. */
    if (hdr->role == (uint8_t)s_role || hdr->role == (uint8_t)WLINK_ROLE_NONE) {
        return;
    }

    if (!s_peer_ready && info->src_addr) {
        wlink_learn_peer(info->src_addr);
    }

    /* Every frame from the peer, beacons included, proves the link is still alive. */
    if (s_peer_ready && info->src_addr && memcmp(info->src_addr, s_peer_mac, ESP_NOW_ETH_ALEN) == 0) {
        s_peer_last_seen_ms = wlink_now_ms() | 1u;
    }

    if (hdr->type == WLINK_MSG_BEACON || hdr->type >= WLINK_MSG_MAX) {
        return;
    }

    const wlink_channel_t channel = WLINK_CHANNEL_FROM_MSG(hdr->type);
    if ((unsigned)channel >= WLINK_CHANNEL_COUNT) {
        return;
    }

    wlink_rx_cb_t cb = s_rx_cb[channel];
    if (cb) {
        cb(channel, hdr->sub, hdr->seq, data + WLINK_HDR_LEN, (size_t)(len - WLINK_HDR_LEN));
    }
}

static void wlink_beacon_cb(void *arg)
{
    (void)arg;

    uint8_t frame[WLINK_HDR_LEN] = { WLINK_MSG_BEACON, (uint8_t)s_role, 0, 0 };
    esp_now_send(s_broadcast_mac, frame, sizeof(frame));
}

esp_err_t wlink_init(wlink_role_t role)
{
    if (role == WLINK_ROLE_NONE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_role != WLINK_ROLE_NONE) {
        ESP_LOGW(TAG, "Already initialised");
        return ESP_OK;
    }

    s_role = role;

    s_tx_mutex = xSemaphoreCreateMutex();
    if (!s_tx_mutex) {
        return ESP_ERR_NO_MEM;
    }

    /* Wi-Fi keeps its calibration data in NVS. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_WLINK_CHANNEL, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(wlink_recv_cb));

    /* Broadcast peer is required to beacon before the peer is known. */
    ESP_ERROR_CHECK(wlink_add_peer(s_broadcast_mac));

    uint8_t own_mac[ESP_NOW_ETH_ALEN] = {0};
    esp_read_mac(own_mac, ESP_MAC_WIFI_STA);

    const esp_timer_create_args_t beacon_args = {
        .callback = wlink_beacon_cb,
        .name = "wlink_beacon",
    };
    ESP_ERROR_CHECK(esp_timer_create(&beacon_args, &s_beacon_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_beacon_timer, WLINK_BEACON_INTERVAL_MS * 1000));

    ESP_LOGI(TAG, "Link up as %s, mac " MACSTR ", channel %d",
             role == WLINK_ROLE_HOST ? "HOST" : "SLAVE", MAC2STR(own_mac), CONFIG_WLINK_CHANNEL);

#if CONFIG_WLINK_DEBUG_LINK_TEST
    err = wlink_start_debug_task();
    if (err != ESP_OK) {
        return err;
    }
#endif

    return ESP_OK;
}

esp_err_t wlink_register_rx(wlink_channel_t channel, wlink_rx_cb_t cb)
{
    if ((unsigned)channel >= WLINK_CHANNEL_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    s_rx_cb[channel] = cb;
    return ESP_OK;
}

esp_err_t wlink_send(wlink_channel_t channel, uint8_t sub, uint8_t seq, const void *payload, size_t len)
{
    if (s_role == WLINK_ROLE_NONE || (unsigned)channel >= WLINK_CHANNEL_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > 0 && payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const wlink_msg_type_t type = (wlink_msg_type_t)WLINK_MSG_FROM_CHANNEL(channel);

    const uint8_t *src = (const uint8_t *)payload;
    size_t offset = 0;
    esp_err_t result = ESP_OK;

    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);

    do {
        uint8_t frame[WLINK_HDR_LEN + WLINK_MAX_PAYLOAD];
        size_t chunk = len - offset;
        if (chunk > WLINK_MAX_PAYLOAD) {
            chunk = WLINK_MAX_PAYLOAD;
        }

        frame[0] = (uint8_t)type;
        frame[1] = (uint8_t)s_role;
        frame[2] = seq;
        frame[3] = sub;
        if (chunk > 0) {
            memcpy(frame + WLINK_HDR_LEN, src + offset, chunk);
        }

        /* Broadcast until the peer is discovered, unicast (with ESP-NOW ACK) afterwards. */
        const uint8_t *dest = s_peer_ready ? s_peer_mac : s_broadcast_mac;
        esp_err_t err = esp_now_send(dest, frame, WLINK_HDR_LEN + chunk);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "esp_now_send(%s) failed: %s", wlink_msg_type_str(type), esp_err_to_name(err));
            if (result == ESP_OK) {
                result = err;
            }
        }

        offset += chunk;
    } while (offset < len);

    xSemaphoreGive(s_tx_mutex);

    return result;
}

#if CONFIG_WLINK_DEBUG_LINK_TEST

/*
 * Link self test.
 *
 * Diagnosing the bridge over ESP-NOW is awkward, because neither board's console shows
 * what the other one sees. With CONFIG_WLINK_DEBUG_LINK_TEST the slave therefore sends a
 * tiny "connected N" line once per second once it has discovered its peer. The line
 * travels on WLINK_CHANNEL_SERIAL, so it follows exactly the same path as the tunnelled UART
 * data and shows up on the serial port of the USB side: that makes it an end to end
 * check of the link, not just of the radio. The value of N counts the lines sent since
 * start-up and never resets.
 *
 * A dedicated task is used instead of an esp_timer callback, because wlink_send() takes
 * the transmit mutex and esp_timer callbacks must never block.
 */
#define WLINK_DEBUG_PERIOD_MS 1000

static void wlink_debug_task(void *arg)
{
    (void)arg;
    int counter = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(WLINK_DEBUG_PERIOD_MS));

        if (!s_peer_ready) {
            continue;   /* nothing to test as long as the peer has not been discovered */
        }

        char msg[32];
        int len = snprintf(msg, sizeof(msg), "connected %d\r\n", ++counter);
        if (len <= 0) {
            continue;
        }
        if (len > (int)sizeof(msg) - 1) {
            len = (int)sizeof(msg) - 1;
        }

        esp_err_t err = wlink_send(WLINK_CHANNEL_SERIAL, 0, 0, msg, (size_t)len);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Link test: connected %d", counter);
        } else {
            ESP_LOGW(TAG, "Link test: cannot send the line: %s", esp_err_to_name(err));
        }
    }
}

static esp_err_t wlink_start_debug_task(void)
{
    if (s_role == WLINK_ROLE_HOST) {
        /* The host side only relays what the slave sends, so there is nothing to start. */
        ESP_LOGI(TAG, "Link test is enabled, but only the wireless slave sends it; "
                 "the host side has nothing to send");
        return ESP_OK;
    }

    if (xTaskCreate(wlink_debug_task, "wlink_debug", 3 * 1024, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Cannot start the link test task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Link test: sending \"connected N\" once per second once the peer is known");
    return ESP_OK;
}

#endif /* CONFIG_WLINK_DEBUG_LINK_TEST */

bool wlink_peer_ready(void)
{
    return s_peer_ready;
}

bool wlink_link_up(void)
{
    const uint32_t last = s_peer_last_seen_ms;

    if (!s_peer_ready || last == 0) {
        return false;
    }

    /* Unsigned difference, so it stays correct when the millisecond counter wraps. */
    return (wlink_now_ms() - last) < CONFIG_WLINK_PEER_TIMEOUT_MS;
}

uint8_t wlink_next_seq(void)
{
    return s_seq++;
}

static const char *wlink_msg_type_str(wlink_msg_type_t type)
{
    switch (type) {
    case WLINK_MSG_BEACON:
        return "BEACON";
    case WLINK_MSG_SERIAL:
        return "SERIAL";
    case WLINK_MSG_SERIAL_CMD:
        return "SERIAL_CMD";
    case WLINK_MSG_SWD:
        return "SWD";
    default:
        return "?";
    }
}
