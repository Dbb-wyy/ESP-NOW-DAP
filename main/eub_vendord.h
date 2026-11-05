/*
 * SPDX-FileCopyrightText: 2020-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/*
 * Phase 4 (B2): the vendor interface and its MS OS 2.0 descriptors moves to components/usb_device.
 */

#include <stddef.h>

#define EUB_VENDORD_EPSIZE          64 /* full speed */
#define EUB_VENDORD_IFACE_SUBCLASS  0xFF
#define EUB_VENDORD_IFACE_PROTOCOL  0x01
#define EUB_VENDORD_IFACE_STR_IDX   5

#define VENDOR_REQUEST_MICROSOFT 0x20  // Can be any value between 0x20 and 0xFF
#define MS_OS_20_DESC_LEN  0xB2

void eub_vendord_start(void);
