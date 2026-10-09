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
 * The link is deliberately tiny: a 4 byte header, a role based peer discovery and a
 * per-message-type dispatch table. Two devices form a pair when they run the opposite
 * roles; both beacon until they have learned the peer MAC and then switch to unicast,
 * which gives hardware ACK/retransmission from ESP-NOW itself.
 */

/** Maximum ESP-NOW v1 payload minus our header. */
#define WLINK_MAX_PAYLOAD   (250 - 4)

/** Size of the fixed link header prepended to every message. */
#define WLINK_HDR_LEN       4

/** Beacons sent until the peer is discovered. */
#define WLINK_BEACON_INTERVAL_MS 1000

typedef enum {
    WLINK_ROLE_NONE = 0,
    WLINK_ROLE_HOST,        /*!< USB side: talks to the host PC */
    WLINK_ROLE_SLAVE,       /*!< Target side: drives UART and SWD pins */
} wlink_role_t;

typedef enum {
    WLINK_MSG_BEACON = 0,   /*!< peer discovery, no payload */
    WLINK_MSG_SERIAL,       /*!< raw serial bytes between PC and target */
    WLINK_MSG_SERIAL_CMD,   /*!< tunneled CDC line coding / line state */
    WLINK_MSG_SWD,          /*!< CMSIS-DAP request / response bytes */
    WLINK_MSG_MAX,
} wlink_msg_type_t;

typedef enum {
    WLINK_SERCMD_LINE_CODING = 0,   /*!< payload: uint32_t baud, little endian */
    WLINK_SERCMD_LINE_STATE  = 1,   /*!< payload: {dtr, rts} */
} wlink_sercmd_t;

/** Fixed header that precedes every link message. */
typedef struct __attribute__((packed)) {
    uint8_t type;   /*!< wlink_msg_type_t */
    uint8_t role;   /*!< wlink_role_t of the sender, used to pair opposite roles only */
    uint8_t seq;    /*!< per-type sequence number, used to match SWD responses */
    uint8_t sub;    /*!< wlink_sercmd_t for WLINK_MSG_SERIAL_CMD, 0 otherwise */
} wlink_hdr_t;

/** Receive handler. Runs in the Wi-Fi task context: it must not block. */
typedef void (*wlink_rx_cb_t)(const wlink_hdr_t *hdr, const uint8_t *payload, size_t len);

/**
 * @brief Initialise Wi-Fi (station, no power save) and ESP-NOW, then start beaconing.
 *
 * @param role Role this device plays on the link.
 * @return ESP_OK on success.
 */
esp_err_t wlink_init(wlink_role_t role);

/**
 * @brief Register the handler for one message type.
 */
esp_err_t wlink_register_rx(wlink_msg_type_t type, wlink_rx_cb_t cb);

/**
 * @brief Send a message to the peer.
 *
 * Payloads larger than WLINK_MAX_PAYLOAD are split into several frames.
 * When the peer has not been discovered yet the frame is broadcast.
 *
 * @param type    Message type, must not be WLINK_MSG_BEACON.
 * @param sub     Sub type, only meaningful for WLINK_MSG_SERIAL_CMD.
 * @param seq     Sequence number, only meaningful for WLINK_MSG_SWD.
 * @param payload Payload, may be NULL when len is 0.
 * @param len     Payload length.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE when no peer is known yet and the
 *         frame was dropped.
 */
esp_err_t wlink_send(wlink_msg_type_t type, uint8_t sub, uint8_t seq, const void *payload, size_t len);

/** @brief true once the opposite role has been discovered. */
bool wlink_peer_ready(void);

/** @brief Copy the peer MAC address into @p out (6 bytes). */
void wlink_peer_mac(uint8_t out[6]);

/** @brief Get the next sequence number for a message type. */
uint8_t wlink_next_seq(void);

/** @brief Role this device was initialised with. */
wlink_role_t wlink_get_role(void);

/** @brief Human readable name of a message type, for logging. */
const char *wlink_msg_type_str(wlink_msg_type_t type);

#ifdef __cplusplus
}
#endif
