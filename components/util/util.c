/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <esp_system.h>
#include "util.h"

void __attribute__((noreturn)) eub_abort(void)
{
    /* Plain abort(): serial logs are the only failure record. The status LED is a separate
       feature (see led_rgb.c) and is deliberately not used to signal an abort. */
    abort();
}
