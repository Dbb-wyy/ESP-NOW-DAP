/*
 * SPDX-FileCopyrightText: 2020-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/*
 * Fatal error handling for the USB and serial bridges.
 *
 * The bridge has no user interface to report a fault through, so the only record is
 * the log line the caller prints right before giving up; eub_abort() just stops the
 * image. It lives in main/ because every caller is here - it used to sit in a
 * "util" component that otherwise held nothing but the status LED driver.
 */

#define GET_BYTE(n, b)  (((n) >> ((b) * 8)) & 0xFF)

void __attribute__((noreturn)) eub_abort(void);
