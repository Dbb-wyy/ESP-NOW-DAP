/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include "eub_abort.h"

void __attribute__((noreturn)) eub_abort(void)
{
    /* Plain abort(): the log line written by the caller is the only failure record.
       The status LED is a separate feature (components/status_led) and is deliberately
       not used to signal an abort. */
    abort();
}
