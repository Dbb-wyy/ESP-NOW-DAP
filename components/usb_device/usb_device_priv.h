/*
 * SPDX-FileCopyrightText: 2026 Dbb
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/*
 * Private interface between the files of components/usb_device. Not in INCLUDE_DIRS.
 */

#include "usb_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback table registered by the bridge (NULL before registration).
 */
const usb_device_ops_t *usb_device_get_ops(void);

/**
 * @brief Push target data into the CDC IN ring buffer (called by usb_device.c).
 */
void serial_bridge_target_data_received(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
