/*
 * SPDX-FileCopyrightText: 2026 Dbb
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/*
 * Private wire format of the ESP-NOW link.
 *
 * This header is not installed and no other component includes it: only
 * wireless_link.c does. Keeping the envelope here makes "callers cannot see frame
 * headers, message types or payload limits" a structural fact, not a convention.
 * The bytes on the wire are unchanged: frame = {type, role, seq, sub} + payload.
 */

#include "wireless_link.h"

/** Message type bytes on the wire. Channels map 1:1 onto them. */
typedef enum {
    WLINK_MSG_BEACON = 0,   /*!< peer discovery, no payload */
    WLINK_MSG_SERIAL,       /*!< WLINK_CHANNEL_SERIAL */
    WLINK_MSG_SERIAL_CMD,   /*!< WLINK_CHANNEL_SERIAL_CMD */
    WLINK_MSG_SWD,          /*!< WLINK_CHANNEL_SWD */
    WLINK_MSG_MAX,
} wlink_msg_type_t;

#define WLINK_CHANNEL_COUNT        3

/** Channel -> wire type, and the reverse (beacons have no channel). */
#define WLINK_MSG_FROM_CHANNEL(ch) ((uint8_t)((ch) + 1))
#define WLINK_CHANNEL_FROM_MSG(t)  ((wlink_channel_t)((t) - 1))

/** Maximum ESP-NOW v1 payload minus the header. */
#define WLINK_MAX_PAYLOAD   (250 - 4)

/** Size of the fixed link header prepended to every message. */
#define WLINK_HDR_LEN       4

/** Beacon period. Beacons are sent for the lifetime of the link. */
#define WLINK_BEACON_INTERVAL_MS 1000

/** Fixed header that precedes every link message. */
typedef struct __attribute__((packed)) {
    uint8_t type;   /*!< wlink_msg_type_t */
    uint8_t role;   /*!< wlink_role_t of the sender, used to pair opposite roles only */
    uint8_t seq;    /*!< per-channel sequence number, used to match SWD responses */
    uint8_t sub;    /*!< sub type, only meaningful for WLINK_MSG_SERIAL_CMD */
} wlink_hdr_t;
