/*
 * SPDX-FileCopyrightText: 2026 Dbb
 *
 * SPDX-License-Identifier: Apache-2.0
 */

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

#include "wireless_link.h"

static const char *TAG = "wlink";

static const uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static wlink_role_t s_role = WLINK_ROLE_NONE;
static wlink_rx_cb_t s_rx_cb[WLINK_MSG_MAX];
static SemaphoreHandle_t s_tx_mutex;

static uint8_t s_peer_mac[ESP_NOW_ETH_ALEN];
static volatile bool s_peer_ready;
static uint8_t s_seq;
static esp_timer_handle_t s_beacon_timer;

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

    if (s_beacon_timer) {
        esp_timer_stop(s_beacon_timer);
    }

    ESP_LOGI(TAG, "Peer discovered: " MACSTR " (peer role: %s)", MAC2STR(s_peer_mac),
             s_role == WLINK_ROLE_HOST ? "slave" : "host");
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

    if (hdr->type == WLINK_MSG_BEACON || hdr->type >= WLINK_MSG_MAX) {
        return;
    }

    wlink_rx_cb_t cb = s_rx_cb[hdr->type];
    if (cb) {
        cb(hdr, data + WLINK_HDR_LEN, (size_t)(len - WLINK_HDR_LEN));
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

    return ESP_OK;
}

esp_err_t wlink_register_rx(wlink_msg_type_t type, wlink_rx_cb_t cb)
{
    if (type >= WLINK_MSG_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    s_rx_cb[type] = cb;
    return ESP_OK;
}

esp_err_t wlink_send(wlink_msg_type_t type, uint8_t sub, uint8_t seq, const void *payload, size_t len)
{
    if (s_role == WLINK_ROLE_NONE || type >= WLINK_MSG_MAX || type == WLINK_MSG_BEACON) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > 0 && payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

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

bool wlink_peer_ready(void)
{
    return s_peer_ready;
}

void wlink_peer_mac(uint8_t out[6])
{
    memcpy(out, s_peer_mac, ESP_NOW_ETH_ALEN);
}

uint8_t wlink_next_seq(void)
{
    return s_seq++;
}

wlink_role_t wlink_get_role(void)
{
    return s_role;
}

const char *wlink_msg_type_str(wlink_msg_type_t type)
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
