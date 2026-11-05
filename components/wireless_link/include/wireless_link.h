/*
 * SPDX-FileCopyrightText: 2026 Dbb
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ESP-NOW link used by the wireless bridge modes.
 *
 * Callers only see logical channels: the frame header, message type bytes, payload
 * limits and beacon period are private to the component. Two devices form a pair when
 * they run opposite link identities; both keep sending a short broadcast beacon
 * forever, so either side can rediscover the other after a reset. Once the peer MAC is
 * known, payload traffic switches to unicast, which gives hardware ACK/retransmission
 * from ESP-NOW itself.
 */

/**
 * Link identity carried on the wire.
 *
 * This is *not* the compile-time bridge role: the bridge decides which identity this
 * device assumes and passes it to wlink_init(). The identity travels in every beacon
 * and unicast frame, so two devices only pair when their identities are opposite.
 */
typedef enum {
    WLINK_ROLE_NONE = 0,
    WLINK_ROLE_HOST,        /*!< USB side: talks to the host PC */
    WLINK_ROLE_SLAVE,       /*!< Target side: drives UART and SWD pins */
} wlink_role_t;

/** Logical channels carried by the link. */
typedef enum {
    WLINK_CHANNEL_SERIAL = 0,   /*!< raw serial bytes between PC and target */
    WLINK_CHANNEL_SERIAL_CMD,   /*!< tunneled CDC line coding / line state */
    WLINK_CHANNEL_SWD,          /*!< CMSIS-DAP request / response bytes */
} wlink_channel_t;

/**
 * Receive handler. Runs in the Wi-Fi task context: it must not block.
 *
 * @param channel Channel the frame arrived on.
 * @param sub     Sub type, only meaningful for WLINK_CHANNEL_SERIAL_CMD.
 * @param seq     Sequence number, only meaningful for WLINK_CHANNEL_SWD.
 * @param payload Payload bytes.
 * @param len     Payload length.
 */
typedef void (*wlink_rx_cb_t)(wlink_channel_t channel, uint8_t sub, uint8_t seq,
                              const uint8_t *payload, size_t len);

/**
 * @brief Initialise Wi-Fi (station, no power save) and ESP-NOW, then start beaconing.
 *
 * @param role Link identity this device plays.
 * @return ESP_OK on success.
 */
esp_err_t wlink_init(wlink_role_t role);

/**
 * @brief Register the handler for one channel.
 */
esp_err_t wlink_register_rx(wlink_channel_t channel, wlink_rx_cb_t cb);

/**
 * @brief Send a message on a channel.
 *
 * Payloads larger than the link frame limit are split into several frames.
 * When the peer has not been discovered yet the frames are broadcast.
 *
 * @param channel Destination channel.
 * @param sub     Sub type, only meaningful for WLINK_CHANNEL_SERIAL_CMD.
 * @param seq     Sequence number, only meaningful for WLINK_CHANNEL_SWD.
 * @param payload Payload, may be NULL when len is 0.
 * @param len     Payload length.
 * @return ESP_OK on success, otherwise the first ESP-NOW error encountered.
 */
esp_err_t wlink_send(wlink_channel_t channel, uint8_t sub, uint8_t seq, const void *payload, size_t len);

/** @brief true once the opposite identity has been discovered. */
bool wlink_peer_ready(void);

/**
 * @brief Whether frames have been received from the peer recently.
 *
 * This is a passive observation for the status LED: it is true while a discovered peer
 * keeps sending beacons or payload, and it turns false CONFIG_WLINK_PEER_TIMEOUT_MS after
 * the last frame. It is deliberately *not* what wlink_peer_ready() means - that one only
 * reports whether the MAC is known and it drives the SWD stop-and-wait retransmission, so
 * it must not start flapping when the peer goes quiet.
 *
 * @return true while the link looks alive.
 */
bool wlink_link_up(void);

/** @brief Get the next sequence number for SWD requests. */
uint8_t wlink_next_seq(void);

#ifdef __cplusplus
}
#endif
