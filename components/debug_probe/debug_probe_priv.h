/*
 * SPDX-FileCopyrightText: 2026 Dbb
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/*
 * Private debug_probe definitions.
 *
 * Nothing in here is part of the component interface: the worker task priority
 * stays behind this header so callers cannot depend on it. The public API in
 * debug_probe.h exposes a command result as (esp_err_t, payload, length).
 */

/* Priority of the debug probe worker task. The USB send task in main runs one
   level below it. */
#define DEBUG_PROBE_TASK_PRI 5
