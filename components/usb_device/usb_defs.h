/*
 * SPDX-FileCopyrightText: 2020-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/*
 * Interface and endpoint numbering of the composite device. Private to
 * components/usb_device.
 */

#include "sdkconfig.h"

// USB Interface Numbers
enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_VENDOR,
    ITF_NUM_TOTAL
};

// USB Endpoint Numbers
#define EPNUM_CDC       2
#define EPNUM_VENDOR    3
