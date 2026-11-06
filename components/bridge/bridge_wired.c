/*
 * SPDX-FileCopyrightText: 2026 Dbb
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Wired role: the bridge owns the target UART and the BOOT/RST lines and reaches
 * the target directly through USB. There is no link, so the status LED has no
 * link state to report and breathes white while the bridge idles.
 */

#include "sdkconfig.h"
#include "esp_err.h"

#include "bridge_priv.h"
#include "status_led.h"

#if BRIDGE_ROLE_ID == BRIDGE_ROLE_WIRED_ID

esp_err_t bridge_wired_init(void)
{
    /* No link to report: the LED breathes white while the bridge idles. */
    status_led_set_local(true);

    return bridge_local_target_start();
}

#endif /* BRIDGE_ROLE_ID == BRIDGE_ROLE_WIRED_ID */
