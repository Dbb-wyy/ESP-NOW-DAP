/*
 * SPDX-FileCopyrightText: 2020-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "sdkconfig.h"

// USB Interface Numbers
enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_VENDOR,
#if CONFIG_BRIDGE_ROLE_WIRED
    /* The wireless roles have no mass storage interface: there the target is flashed
       through the tunnelled serial port with the host side esptool. */
    ITF_NUM_MSC,
#endif
    ITF_NUM_TOTAL
};

// USB Endpoint Numbers
#define EPNUM_CDC       2
#define EPNUM_VENDOR    3
#define EPNUM_MSC       4
